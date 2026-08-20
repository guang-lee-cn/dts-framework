#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
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

// oam 远程命令响应（msg13：control 线程 DDS 控制通道执行结果，载荷 "cmd=<line>\n<out>"）
std::mutex g_oamCmdM;
std::string g_oamCmdResp;
std::atomic<int> g_oamCmdCount{0};

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

// oam 远程命令响应回调（msg13：control 线程 DDS 控制通道，P1-3）
void OnOamCmdResp(void*, std::unique_ptr<std::vector<uint8_t>> data) {
    if (!data || data->empty()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_oamCmdM);
        g_oamCmdResp.assign(data->begin(), data->end());
    }
    g_oamCmdCount.fetch_add(1);
    std::printf("[spa] oam cmd response (%u bytes):\n",
                static_cast<uint32_t>(data->size()));
    std::fflush(stdout);
}

// 远程命令往返：发命令行（msg12）-> 等响应（msg13）。重发自愈（发现窗口期消息可丢）；
// 超时返回空串
std::string OamCmdRoundtrip(detmw::Communicator& comm, const char* cmd) {
    g_oamCmdCount.store(0);
    const std::string line = cmd;
    for (int i = 0; i < 3 && g_oamCmdCount.load() == 0; ++i) {
        if (comm.publish_external(detmw::endpoint{SESSION_TYPE_DTS, SESSION_INST_OAM,
                                                  MSG_ID_OAM_CMD_REQ},
                                  reinterpret_cast<const uint8_t*>(line.data()),
                                  static_cast<uint32_t>(line.size())) != 0) {
            return "";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    for (int i = 0; i < 30 && g_oamCmdCount.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::lock_guard<std::mutex> lk(g_oamCmdM);
    return g_oamCmdResp;
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
// → 自收上报(msg4) 断言端到端 data 链路（真实消费，非仅投递）
// → oam 指标拉取（msg10/11，data 线程 oam 组）→ 远程命令往返（msg12/13，control 线程）。
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
    // 订阅 oam 远程命令响应（msg13：control 线程 DDS 控制通道）
    if (comm.subscribe(detmw::endpoint{SESSION_TYPE_DTS, SESSION_INST_OAM, MSG_ID_OAM_CMD_RESP},
                       OnOamCmdResp, nullptr) != 0) {
        std::printf("[spa] subscribe oam cmd resp failed\n");
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

    // ---- 远程控制通道（P1-3，D2/R5）：DDS 命令行 -> control(CommandExecutor) -> 响应 ----
    // 正常路径：get_data_stats（响应须带 cmd 回显前缀 + frames= 字段）
    std::printf("[spa] oam remote cmd (DTS.oam.msg12 -> control)...\n");
    const std::string r1 = OamCmdRoundtrip(comm, "get_data_stats");
    if (!r1.empty()) {
        std::fwrite(r1.data(), 1, r1.size() < 512 ? r1.size() : 512, stdout);
        std::printf("\n");
    }
    const bool cmdOk = r1.find("cmd=get_data_stats") != std::string::npos &&
                       r1.find("frames=") != std::string::npos;
    std::printf("[spa] %s\n",
                cmdOk ? "PASS (remote cmd via control verified)"
                      : "FAIL (remote cmd no/invalid response)");
    // 错误路径：未知命令（Execute 拒绝，响应含 not found）
    const std::string r2 = OamCmdRoundtrip(comm, "no_such_cmd_test");
    const bool cmdErrOk = r2.find("not found:") != std::string::npos;
    std::printf("[spa] %s\n",
                cmdErrOk ? "PASS (unknown cmd rejected with error)"
                         : "FAIL (unknown cmd error path)");
    return (ok && oamOk && cmdOk && cmdErrOk) ? 0 : 1;
}
