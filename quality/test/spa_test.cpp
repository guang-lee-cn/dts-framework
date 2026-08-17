#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "detmw.h"
#include "dts_def.h"

// 复用 data domain 的 dataId 大小表（spa/data 共享 raw layout，见 data_ids.h）
#include "data_ids.h"

using namespace dts;

namespace {

// 小帧 raw（跨进程测试用，避免 32K 依赖 SHM/MTU）：head + 前 kSlices 个切片
constexpr uint32_t kRawHead = sizeof(uint16_t) + sizeof(uint32_t) * 2;  // dataType+cellId+cpId = 10
constexpr uint16_t kSlices = 8;  // 只发前 8 个 dataId 切片（其余越界由工厂跳过）

std::atomic<int> g_reportCount{0};
std::atomic<int> g_oamRespCount{0};

// 上报回调（msg4：data 线程合并上报出口）
void OnReport(void*, std::unique_ptr<std::vector<uint8_t>> data) {
    g_reportCount.fetch_add(1);
    std::printf("[spa] report received (%u bytes)\n",
                data ? static_cast<uint32_t>(data->size()) : 0);
    std::fflush(stdout);
}

// oam 指标响应回调（msg11：data 线程 oam 业务组拉取响应）
void OnOamStatsResp(void*, std::unique_ptr<std::vector<uint8_t>> data) {
    g_oamRespCount.fetch_add(1);
    const uint32_t n = data ? static_cast<uint32_t>(data->size()) : 0;
    std::printf("[spa] oam stats response (%u bytes):\n", n);
    if (data && n > 0) {
        std::fwrite(data->data(), 1, n < 512 ? n : 512, stdout);
        std::printf("\n");
    }
    std::fflush(stdout);
}

// 构造 raw 帧：新布局 [u16 dataType=CELL][u32 cellId][u32 cpId][Σ切片]
// （替代旧 BigFrameHeader 布局；越界切片由 DataFactory 跳过，见 data_factory_v2.cpp）
void BuildRaw(std::vector<uint8_t>& out) {
    uint32_t len = kRawHead;
    for (uint16_t i = 0; i < kSlices; ++i) {
        len += dts::data::kCellCacheSizes[i];
    }
    out.resize(len, 0);
    const uint16_t dataType = static_cast<uint16_t>(dts::data::DataType::CELL);
    std::memcpy(out.data(), &dataType, sizeof(dataType));
    const uint32_t cellId = 100;
    std::memcpy(out.data() + sizeof(uint16_t), &cellId, sizeof(cellId));
    const uint32_t cpId = 0;
    std::memcpy(out.data() + sizeof(uint16_t) + sizeof(uint32_t), &cpId, sizeof(cpId));
    uint32_t off = kRawHead;
    for (uint16_t i = 0; i < kSlices; ++i) {
        std::memset(out.data() + off, static_cast<int>(i), dts::data::kCellCacheSizes[i]);
        off += dts::data::kCellCacheSizes[i];
    }
}

}  // namespace

// spa 测试进程：握手（TaskRequest → data 建任务）→ 发 rawData(msg3, 新布局小帧)
// → 自收上报(msg4) 断言端到端 data 链路（真实消费，非仅投递）。
// 用法：spa_test <spa.json>
int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: %s <spa.json>\n", argv[0]);
        return 1;
    }
    detmw::Communicator comm(argv[1]);

    // 订阅上报（msg4：data 线程合并上报出口）
    if (comm.subscribe(detmw::endpoint{SESSION_TYPE_DTS, SESSION_INST_DATA, MSG_ID_REPORT},
                       OnReport, nullptr) != 0) {
        std::printf("[spa] subscribe report failed\n");
        return 1;
    }
    // 订阅 oam 指标响应（msg11：data 线程 oam 业务组）
    if (comm.subscribe(detmw::endpoint{SESSION_TYPE_DTS, SESSION_INST_OAM, MSG_ID_OAM_STATS_RESP},
                       OnOamStatsResp, nullptr) != 0) {
        std::printf("[spa] subscribe oam resp failed\n");
        return 1;
    }

    std::printf("[spa] waiting for static discovery...\n");
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // 握手：通知 data 当前任务（taskId=1, status=1）。重发自愈（发现完成前消息会被丢）
    const TaskRequest req{1, 1};
    for (int i = 0; i < 3; ++i) {
        const int rc = comm.publish_external(detmw::endpoint{SESSION_TYPE_DTS, SESSION_INST_DATA,
                                                             MSG_ID_DATA_TASK_ACTIVE},
                                             reinterpret_cast<const uint8_t*>(&req), sizeof(req));
        if (rc != 0) {
            std::printf("[spa] handshake publish failed\n");
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    std::printf("[spa] handshake sent (taskId=1), wait 2s for data OnTask...\n");
    std::this_thread::sleep_for(std::chrono::seconds(2));  // 等 data 建 currentTask

    // raw 帧：重发自愈（发现完成前发出的首帧可能被丢；data 处理幂等）
    std::vector<uint8_t> raw;
    BuildRaw(raw);
    std::printf("[spa] sending %zu bytes rawData to data thread (msg3, %u slices)\n",
                raw.size(), kSlices);
    for (int i = 0; i < 3 && g_reportCount.load() == 0; ++i) {
        const int rc = comm.publish_external(detmw::endpoint{SESSION_TYPE_DTS, SESSION_INST_DATA,
                                                             MSG_ID_AGENT_DATA},
                                             raw.data(), static_cast<uint32_t>(raw.size()));
        if (rc != 0) {
            std::printf("[spa] raw publish failed\n");
            return 1;
        }
        if (g_reportCount.load() == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

    // 等上报（可靠 QoS，发现后不丢）
    std::printf("[spa] waiting for report (msg4)...\n");
    for (int i = 0; i < 50 && g_reportCount.load() == 0; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    const bool ok = g_reportCount.load() > 0;
    std::printf("[spa] %s\n", ok ? "PASS (handshake+feed+report link verified)" : "FAIL (no report)");

    // ---- oam 业务组拉取（P1-2 指标出口）：发 OAM_STATS_REQ → 收 OAM_STATS_RESP ----
    std::printf("[spa] oam stats pull (DTS.oam)...\n");
    g_oamRespCount.store(0);
    const uint8_t reqBody[1] = {0x01};  // 非空载荷（FastDDS 对零长度样本投递不可靠）
    for (int i = 0; i < 3 && g_oamRespCount.load() == 0; ++i) {
        const int reqRc = comm.publish_external(detmw::endpoint{SESSION_TYPE_DTS,
                                                                SESSION_INST_OAM,
                                                                MSG_ID_OAM_STATS_REQ},
                                                reqBody, sizeof(reqBody));
        if (reqRc != 0) {
            std::printf("[spa] oam req publish failed\n");
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    for (int i = 0; i < 30 && g_oamRespCount.load() == 0; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    const bool oamOk = g_oamRespCount.load() > 0;
    std::printf("[spa] %s\n",
                oamOk ? "PASS (oam stats pull verified)" : "FAIL (no oam stats response)");
    return (ok && oamOk) ? 0 : 1;
}
