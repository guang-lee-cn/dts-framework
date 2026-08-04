#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include "detmw.h"
#include "dts_def.h"

using namespace dts;

namespace {
std::atomic<int> g_replyCount{0};

// 收到 task 回包（msg2：task 现有 handler 收到 msg1 后 forward）
void OnReply(void*, const uint8_t*, uint32_t) {
    g_replyCount.fetch_add(1);
    std::printf("[nfoam] task reply received\n");
    std::fflush(stdout);
}
}  // namespace

// nfoam 测试进程：向 dts task 发任务建立请求(msg1)，等回包(msg2)
// 用法：nfoam_test <nfoam.json>
int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: %s <nfoam.json>\n", argv[0]);
        return 1;
    }
    detmw_handle* h = detmw_init(argv[1]);
    if (h == nullptr) return 1;

    if (detmw_subscribe(h, SESSION_TYPE_DTS, SESSION_INST_DATA, MSG_ID_DATA_TASK_ACTIVE, OnReply,
                        nullptr) != 0) {
        std::printf("[nfoam] subscribe reply failed\n");
        detmw_destroy(h);
        return 1;
    }

    std::printf("[nfoam] waiting for static discovery...\n");
    std::this_thread::sleep_for(std::chrono::seconds(3));

    TaskActiveMsg m{};
    m.taskId = 1;
    m.type = 0;
    m.dataIdCount = 1;
    m.dataIds[0] = DATA_ID_CELL_PRB;
    std::printf("[nfoam] sending task-establish request (msg1)\n");
    int rc = detmw_publish(h, SESSION_TYPE_DTS, SESSION_INST_TASK, MSG_ID_TASK_ACTIVE,
                           reinterpret_cast<const uint8_t*>(&m), sizeof(m));
    if (rc != 0) {
        std::printf("[nfoam] publish failed\n");
        detmw_destroy(h);
        return 1;
    }

    // 等 task 回包（可靠 QoS，发现后不丢）
    for (int i = 0; i < 50 && g_replyCount.load() == 0; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    bool ok = g_replyCount.load() > 0;
    detmw_destroy(h);
    std::printf("[nfoam] %s\n", ok ? "PASS" : "FAIL (no task reply)");
    return ok ? 0 : 1;
}
