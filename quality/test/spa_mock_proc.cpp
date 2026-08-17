// spaMock 独立进程（DDS 调优链路）：握手 + 上报大结构 rawData 给 dts data 线程。
// raw 布局（对齐 data 工厂 500 dataId 切分）：
//   [u16 dataType=0(CELL)][u32 cellId][u32 cpId][500 × 8字节切片]  ≈ 4KB
// extractor i 切 head + i*8（见 extractors.cpp）
// 用法：spa_mock_proc <tune-spa.json> [rounds] [interval_us]
//   rounds=0 → 持续发到收到信号（稳态测量用）；interval_us=0 → 全速
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "detmw.h"
#include "dts_def.h"

// 复用 data domain 的 dataId 大小表（spa/data 共享 raw layout，Σ cacheSize = 32K-head）
#include "data_ids.h"

using namespace dts;

namespace {

constexpr uint32_t kRawHead = sizeof(uint16_t) + sizeof(uint32_t) * 2;  // dataType + cellId + cpId = 10

std::atomic<bool> g_stop{false};
void OnSig(int) { g_stop.store(true); }

uint64_t NowUs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// 构造 raw 帧：head + 500 变长切片（大小取自 kCellCacheSizes，偏移与 data extractor 对齐）。
// rawLen 默认 kRawLen（32K）；受限环境（无 SHM/UDP 大包不可达）用 DTS_RAW_LEN 调小，
// 越界切片由 DataFactory 跳过（见 data_factory_v2.cpp Extra<0 分支）。
void BuildRaw(std::vector<uint8_t>& out, uint32_t cellId, uint32_t seq, uint32_t rawLen) {
    out.resize(rawLen);
    uint16_t dataType = 0;  // CELL
    std::memcpy(out.data(), &dataType, sizeof(dataType));
    std::memcpy(out.data() + sizeof(uint16_t), &cellId, sizeof(cellId));
    uint32_t cpId = 0;
    std::memcpy(out.data() + sizeof(uint16_t) + sizeof(uint32_t), &cpId, sizeof(cpId));
    // 500 切片：每切片按 kCellCacheSizes[i] 大小，填 seq 字节（0 填充）便于校验；
    // 帧内放不下的切片不写（工厂侧越界跳过）
    uint32_t off = kRawHead;
    for (uint16_t i = 0; i < dts::data::kCellDataIdCount; ++i) {
        const uint16_t sz = dts::data::kCellCacheSizes[i];
        if (off + sz > rawLen) {
            break;
        }
        std::memset(out.data() + off, static_cast<int>(seq & 0xff), sz);
        out[off] = static_cast<uint8_t>(i & 0xff);  // 首 byte = dataId 序号
        off += sz;
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: %s <tune-spa.json> [rounds] [interval_us]\n", argv[0]);
        return 1;
    }
    const long rounds = (argc >= 3) ? std::atol(argv[2]) : 1000;  // 0 = 持续到信号
    const int intervalUs = (argc >= 4) ? std::atoi(argv[3]) : 0;
    // raw 帧长：默认 32K；受限环境（无 /dev/shm，UDP 大包不可达）用 DTS_RAW_LEN 调小
    const char* rawLenEnv = std::getenv("DTS_RAW_LEN");
    const uint32_t rawLen = rawLenEnv != nullptr
                                ? static_cast<uint32_t>(std::strtoul(rawLenEnv, nullptr, 10))
                                : dts::data::kRawLen;
    const bool steady = (rounds == 0);
    if (steady) {
        std::signal(SIGINT, OnSig);
        std::signal(SIGTERM, OnSig);
    }

    detmw::Communicator comm(argv[1]);
    if (!comm.good()) {
        std::printf("[spa-mock] communicator init failed\n");
        return 1;
    }

    std::printf("[spa-mock] waiting for static discovery... (raw=%uB, slices=%u)\n", rawLen, dts::data::kCellDataIdCount);
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // 握手：通知 data 当前任务（taskId=1, status=1）
    TaskRequest req{1, 1};
    comm.publish_external(detmw::endpoint{SESSION_TYPE_DTS, SESSION_INST_DATA, MSG_ID_DATA_TASK_ACTIVE},
                          reinterpret_cast<const uint8_t*>(&req), sizeof(req));
    std::printf("[spa-mock] handshake sent (taskId=1), wait 2s for data OnTask...\n");
    std::this_thread::sleep_for(std::chrono::seconds(2));  // 等 data 建 currentTask（避免 raw 抢先到被丢）
    std::printf("[spa-mock] starting %s\n", steady ? "steady stream" : "burst");

    const auto ep = detmw::endpoint{SESSION_TYPE_DTS, SESSION_INST_DATA, MSG_ID_AGENT_DATA};
    std::vector<uint8_t> raw;
    const uint64_t t0 = NowUs();
    uint64_t sent = 0;
    uint32_t fail = 0;
    uint32_t cellId = 1;
    while (!g_stop.load()) {
        BuildRaw(raw, 1 + (cellId % 10), static_cast<uint32_t>(sent), rawLen);
        if (comm.publish_external(ep, raw.data(), rawLen) != 0) {
            ++fail;
        }
        ++sent;
        ++cellId;
        if (!steady && sent >= static_cast<uint64_t>(rounds)) {
            break;
        }
        if (intervalUs > 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(intervalUs));
        }
    }
    const uint64_t dur = NowUs() - t0;
    std::this_thread::sleep_for(std::chrono::seconds(1));  // 排空可靠投递

    const double durS = static_cast<double>(dur) / 1e6;
    const double mbps = static_cast<double>(sent) * rawLen / durS / (1024 * 1024);
    std::printf("[spa-mock] sent %llu frames (%.0f MB) in %.3fs -> %.0f msg/s, %.2f MB/s, fail=%u\n",
                static_cast<unsigned long long>(sent), static_cast<double>(sent) * rawLen / (1024 * 1024),
                durS, sent / durS, mbps, fail);
    return 0;
}
