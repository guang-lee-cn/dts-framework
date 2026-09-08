// librdkafka 薄壳（web-server 专用）：producer 异步投递 + consumer 免组协议消费。
// 依赖：thirdparty/librdkafka（apt download 解包，免 root；缺失时跑 fetch.sh）
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

// librdkafka C API（薄壳直接持有句柄，不引 C++ 绑定层）
#include <librdkafka/rdkafka.h>

namespace web {

// producer：detmw 接收回调线程调用 Produce（异步、非阻塞），发送结果走 delivery 回调
class KafkaProducer {
public:
    KafkaProducer(const std::string& brokers, const std::string& topic);
    ~KafkaProducer();

    KafkaProducer(const KafkaProducer&) = delete;
    KafkaProducer& operator=(const KafkaProducer&) = delete;

    // 连通性判定：metadata 请求 broker，超时(5s)失败 —— fail-fast 语义（对齐 DETMW_BUCKET）
    int Start();

    // 异步投递；返回 0 = 已入队（非已送达）。key=taskId，headers 供 kcat/rpk 快速检视
    int Produce(const uint8_t* data, uint32_t len, const std::string& key,
                const std::vector<std::pair<std::string, std::string>>& headers);

    // 队列丢弃计数（broker 断连且内部队列满时 rd_kafka_produce 失败）
    uint64_t queued() const { return m_queued.load(); }
    uint64_t delivered() const { return m_delivered.load(); }
    uint64_t failed() const { return m_failed.load(); }

    void Shutdown();  // flush(3s) + destroy

private:
    static void OnDelivery(rd_kafka_t* rk, const rd_kafka_message_t* msg, void* ctx);

    std::string m_brokers;
    std::string m_topic;
    rd_kafka_t* m_rk = nullptr;
    rd_kafka_topic_t* m_rkt = nullptr;
    std::atomic<uint64_t> m_queued{0};
    std::atomic<uint64_t> m_delivered{0};
    std::atomic<uint64_t> m_failed{0};
};

// consumer：免 consumer-group 协议（无 coordinator/rebalance），直接 assign 全分区、
// 从最新 offset 起消费 —— 观测面语义：只看启动之后的实时数据（历史回看用 rpk）。
class KafkaConsumer {
public:
    // 回调在消费线程执行；offset/partition 供 UI 展示消费进度
    using MsgFn = std::function<void(const uint8_t* data, uint32_t len, int64_t offset)>;

    KafkaConsumer(const std::string& brokers, const std::string& topic);
    ~KafkaConsumer();

    KafkaConsumer(const KafkaConsumer&) = delete;
    KafkaConsumer& operator=(const KafkaConsumer&) = delete;

    int Start(MsgFn fn);  // 建句柄 + assign 全分区(OFFSET_END) + 起 poll 线程
    void Stop();

    int64_t consumed() const { return m_consumed.load(); }
    int64_t lag();         // 高水位 - 已消费（近似，1s 内刷新）
    bool healthy() const { return m_running.load(); }

private:
    void Run();

    std::string m_brokers;
    std::string m_topic;
    rd_kafka_t* m_rk = nullptr;
    MsgFn m_fn;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<int64_t> m_consumed{0};
    std::atomic<int64_t> m_lag{0};
    std::vector<int> m_parts;         // assign 到的分区号（lag 水位刷新用）
    int64_t m_hiBase = 0;             // 启动时高水位基线（OFFSET_END 语义：只看增量）
};

}  // namespace web
