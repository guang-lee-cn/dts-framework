#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "detmw.h"
#include "dts_def.h"

using namespace dts;

namespace {
std::atomic<int> g_replyCount{0};

// 收到 task 回包（msg8：task OnTaskConfig 处理后回显，真实外部响应通道）
void OnReply(void*, std::unique_ptr<std::vector<uint8_t>> data) {
    const uint32_t n = data ? static_cast<uint32_t>(data->size()) : 0;
    g_replyCount.fetch_add(1);
    std::printf("[nfoam] task reply received (%u bytes)\n", n);
    std::fflush(stdout);
}
}  // namespace

// nfoam 测试进程：向 dts task 发配置变更请求(msg7, JSON)，等回包(msg8)。
// 注意：task→data 的 msg2 已按 D7/D8 走进程内 mailbox 直通（外部不可见），
// 外部往返以 msg7/msg8 为准（与 bench_nfoam 同通道）。
// 用法：nfoam_test <nfoam.json>
int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: %s <nfoam.json>\n", argv[0]);
        return 1;
    }
    detmw::Communicator comm(argv[1]);

    if (comm.subscribe(detmw::endpoint{SESSION_TYPE_DTS, SESSION_INST_TASK, MSG_ID_TASK_RESPONSE},
                       OnReply, nullptr) != 0) {
        std::printf("[nfoam] subscribe reply failed\n");
        return 1;
    }

    std::printf("[nfoam] waiting for static discovery...\n");
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // 32K JSON 配置载荷（真实工况：task 简析 JSON + 回显）。
    // 受限环境（无 /dev/shm，UDP 大包不可达，如 CI 沙箱）用 DTS_CFG_LEN 调小；
    // 32K 大包 S 级回归由 run_slevel.sh（tune 链路）覆盖。
    const char* lenEnv = std::getenv("DTS_CFG_LEN");
    const uint32_t kCfgLen = lenEnv != nullptr ? static_cast<uint32_t>(std::strtoul(lenEnv, nullptr, 10))
                                               : 32 * 1024;
    std::vector<uint8_t> cfg(kCfgLen, 'a');
    const char* head = "{\"taskId\":1,\"seq\":1,\"t_send\":0,\"pad\":\"";
    std::memcpy(cfg.data(), head, std::strlen(head));
    cfg[kCfgLen - 3] = '"';
    cfg[kCfgLen - 2] = '}';
    cfg[kCfgLen - 1] = '\0';

    // 发送重试：发现完成前发出的消息会被 reliable QoS 丢弃（沙箱/容器发现时序波动），
    // 重发自愈；task 处理幂等（重复 config 仅多打日志）
    std::printf("[nfoam] sending task-config request (msg7, %uB)\n", kCfgLen);
    for (int i = 0; i < 3 && g_replyCount.load() == 0; i++) {
        const int rc = comm.publish_external(detmw::endpoint{SESSION_TYPE_DTS, SESSION_INST_TASK,
                                                             MSG_ID_TASK_CONFIG},
                                             cfg.data(), kCfgLen);
        if (rc != 0) {
            std::printf("[nfoam] publish failed\n");
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // 等 task 回包（可靠 QoS，发现后不丢）
    for (int i = 0; i < 50 && g_replyCount.load() == 0; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    bool ok = g_replyCount.load() > 0;
    std::printf("[nfoam] %s\n", ok ? "PASS" : "FAIL (no task reply)");
    return ok ? 0 : 1;
}
