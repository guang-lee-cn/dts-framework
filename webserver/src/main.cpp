// web-server 独立进程：dts 上报（DTS_data_4）→ Kafka 沉淀 → 浏览器实时观测。
// 数据流：detmw 订阅回调 → librdkafka producer（异步）→ topic dts.report；
//        librdkafka consumer（免组协议，OFFSET_END 实时）→ 解析 → SSE 推浏览器。
// 观测即验证沉淀：UI 展示的是从 Kafka 消费回来的数据，不是 DDS 旁路。
//
// 用法：web_server <cfg.json> [brokers=localhost:9092] [http_port=8080]
//                 [topic=dts.report] [web_dir=./web]
// 依赖：broker 不可达 → 启动失败退出非 0（kafka 是显式依赖，fail-fast 不静默）
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "detmw.h"
#include "dts_def.h"
#include "http_sse.h"
#include "kafka_client.h"
#include "log.h"
#include "report_frame.h"
#include "stats.h"

using namespace dts;

namespace {

std::atomic<bool> g_stop{false};

void OnSignal(int) {
    g_stop.store(true);
}

web::Stats g_stats;
std::chrono::steady_clock::time_point g_lastFramePush{};
web::KafkaProducer* g_producer = nullptr;  // main 装配后不变（recv_fn 是函数指针，走全局）

// detmw 接收线程：上报帧 → kafka（异步投递，回调立即返回，不阻塞接收）
void OnDtsReport(void*, std::unique_ptr<std::vector<uint8_t>> data) {
    if (data == nullptr || data->empty() || g_producer == nullptr) {
        return;
    }
    web::ReportView v;
    web::ParseReport(data->data(), static_cast<uint32_t>(data->size()), v);
    char seqStr[16];
    char tsStr[24];
    std::snprintf(seqStr, sizeof(seqStr), "%u", v.seq);
    std::snprintf(tsStr, sizeof(tsStr), "%llu",
                  static_cast<unsigned long long>(v.timestampMs));
    g_producer->Produce(data->data(), static_cast<uint32_t>(data->size()),
                        std::to_string(v.taskId), {{"seq", seqStr}, {"ts_ms", tsStr}});
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: %s <cfg.json> [brokers=localhost:9092] [http_port=8080] "
                    "[topic=dts.report] [web_dir=./web]\n", argv[0]);
        return 1;
    }
    const char* cfg = argv[1];
    const char* brokers = argc >= 3 ? argv[2] : "localhost:9092";
    const uint16_t port = argc >= 4 ? static_cast<uint16_t>(std::atoi(argv[3])) : 8080;
    const char* topic = argc >= 5 ? argv[4] : "dts.report";
    std::string webDir = argc >= 6 ? argv[5] : "./web";

    dts::log::Init();
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    // 1. 通信站点（detmw 对端：订阅 dts 上报）
    detmw::Communicator comm(cfg);
    if (!comm.good()) {
        dts::log::Error("[web] detmw init failed (cfg={})", cfg);
        return 1;
    }

    // 2. kafka producer（fail-fast：broker 不在就退）
    web::KafkaProducer producer(brokers, topic);
    if (producer.Start() != 0) {
        return 1;
    }

    // 3. kafka consumer（同 topic；回调在消费线程：解析 → 统计 → 节流 SSE）
    web::HttpSseServer http;
    web::KafkaConsumer consumer(brokers, topic);
    if (consumer.Start([&http](const uint8_t* data, uint32_t len, int64_t offset) {
            web::ReportView v;
            if (!web::ParseReport(data, len, v)) {
                return;
            }
            g_stats.OnReport(v);
            // 帧明细节流 ≥100ms（高 fps 时不打爆浏览器；曲线数据走 1Hz stats 事件）
            const auto now = std::chrono::steady_clock::now();
            if (now - g_lastFramePush >= std::chrono::milliseconds(100)) {
                g_lastFramePush = now;
                char ev[256];
                std::snprintf(ev, sizeof(ev),
                              "{\"seq\":%u,\"task\":%u,\"ts_ms\":%llu,\"slices\":%u,"
                              "\"data_ids\":[%u,%u],\"len\":%u,\"offset\":%lld}",
                              v.seq, v.taskId,
                              static_cast<unsigned long long>(v.timestampMs), v.sliceCount,
                              v.dataIdMin, v.dataIdMax, v.frameLen,
                              static_cast<long long>(offset));
                http.Broadcast("frame", ev);
            }
        }) != 0) {
        return 1;  // topic 尚无数据（未自动创建）也视为装配失败：先有链路再有观测
    }

    // 4. HTTP/SSE（观测页 + 接口）
    if (http.Start(port, webDir + "/index.html",
                   [] { return g_stats.SnapshotJson(); }) != 0) {
        return 1;
    }

    // 5. 订阅 dts 上报（回调内异步 produce；broker 短暂断连由 librdkafka 内部重试）
    g_producer = &producer;
    if (comm.subscribe(detmw::endpoint{SESSION_TYPE_DTS, SESSION_INST_DATA, MSG_ID_REPORT},
                       OnDtsReport, nullptr) != 0) {
        dts::log::Error("[web] subscribe DTS_data_4 failed");
        return 1;
    }

    dts::log::Info("[web] up (cfg={} brokers={} topic={} http={})", cfg, brokers, topic, port);

    // 6. 常驻：1Hz 统计日志 + SSE stats 推送
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        dts::log::Info("[web] {}", g_stats.SnapshotJson());
        http.Broadcast("stats", g_stats.SnapshotJson());
    }

    dts::log::Info("[web] shutting down");
    consumer.Stop();
    producer.Shutdown();  // flush 等 in-flight 投递完成
    http.Stop();
    dts::log::Shutdown();
    return 0;
}
