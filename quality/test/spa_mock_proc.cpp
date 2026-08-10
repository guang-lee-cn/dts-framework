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

using namespace dts;

namespace {

constexpr uint32_t kSliceCount = 500;
constexpr uint32_t kSliceSize = 8;
constexpr uint32_t kRawHead = sizeof(uint16_t) + sizeof(uint32_t) * 2;  // dataType + cellId + cpId
constexpr uint32_t kRawLen = kRawHead + kSliceCount * kSliceSize;       // ≈ 4010 字节

std::atomic<bool> g_stop{false};
void OnSig(int) { g_stop.store(true); }

uint64_t NowUs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// 构造 raw 帧：dataType + cellId + cpId + 500 切片（每切片填序号 i 用于校验）
void BuildRaw(std::vector<uint8_t>& out, uint32_t cellId, uint32_t seq) {
    out.resize(kRawLen);
    uint16_t dataType = 0;  // CELL
    std::memcpy(out.data(), &dataType, sizeof(dataType));
    std::memcpy(out.data() + sizeof(uint16_t), &cellId, sizeof(cellId));
    uint32_t cpId = 0;
    std::memcpy(out.data() + sizeof(uint16_t) + sizeof(uint32_t), &cpId, sizeof(cpId));
    // 500 切片：每切片 8 字节，填 (seq, i) 便于校验
    for (uint32_t i = 0; i < kSliceCount; ++i) {
        uint32_t off = kRawHead + i * kSliceSize;
        uint32_t v0 = seq;
        uint32_t v1 = i;
        std::memcpy(out.data() + off, &v0, 4);
        std::memcpy(out.data() + off + 4, &v1, 4);
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

    std::printf("[spa-mock] waiting for static discovery... (raw=%uB, slices=%u)\n", kRawLen, kSliceCount);
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // 握手：通知 data 当前任务（taskId=1, status=1）
    TaskRequest req{1, 1};
    comm.publish_external(detmw::endpoint{SESSION_TYPE_DTS, SESSION_INST_DATA, MSG_ID_DATA_TASK_ACTIVE},
                          reinterpret_cast<const uint8_t*>(&req), sizeof(req));
    std::printf("[spa-mock] handshake sent (taskId=1), starting %s\n", steady ? "steady stream" : "burst");

    const auto ep = detmw::endpoint{SESSION_TYPE_DTS, SESSION_INST_DATA, MSG_ID_AGENT_DATA};
    std::vector<uint8_t> raw;
    const uint64_t t0 = NowUs();
    uint64_t sent = 0;
    uint32_t fail = 0;
    uint32_t cellId = 1;
    while (!g_stop.load()) {
        BuildRaw(raw, 1 + (cellId % 10), static_cast<uint32_t>(sent));
        if (comm.publish_external(ep, raw.data(), kRawLen) != 0) {
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
    const double mbps = static_cast<double>(sent) * kRawLen / durS / (1024 * 1024);
    std::printf("[spa-mock] sent %llu frames (%.0f MB) in %.3fs -> %.0f msg/s, %.2f MB/s, fail=%u\n",
                static_cast<unsigned long long>(sent), static_cast<double>(sent) * kRawLen / (1024 * 1024),
                durS, sent / durS, mbps, fail);
    return 0;
}
