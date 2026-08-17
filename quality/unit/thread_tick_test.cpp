// 单元自测：ThreadRun 维护节拍（绝对期限）——持续消息负载下 TIMER 仍准点投递。
// 回归背景：相对期限 wait_until(now+100ms) 在持续负载（mailbox 恒非空）下永不超时，
// TIMER 永不投递 → TTL 回收等维护任务停摆（data 池满静默丢帧的根因之一）。
// 运行：构建后直接执行；退出码 0 = 全过。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include "dts_thread.h"

using namespace dts;

namespace {

std::atomic<int> g_ticks{0};
std::atomic<uint64_t> g_processed{0};

// 线程入口：TIMER = 维护节拍；其它 = 业务消息
void Entry(ThreadStatus, const char*, uint32_t msgId, void*, uint32_t) {
    if (msgId == MSG_ID_TIMER) {
        g_ticks.fetch_add(1);
    } else {
        g_processed.fetch_add(1);
    }
}

}  // namespace

int main() {
    ThreadCtx ctx;
    ctx.Init("tick_test", Entry);

    std::thread run(ThreadRun, &ctx);

    // 持续消息负载：每 5ms 一条（共 1.5s）→ mailbox 几乎恒非空。
    // 旧实现（相对期限）下：每次被消息唤醒后期限顺延 100ms → TIMER 永不触发。
    std::thread feeder([&] {
        for (int i = 0; i < 300; ++i) {
            ctx.m_mailbox.Send(0x0001, nullptr, 0, "task");
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    ctx.RequestStop();
    run.join();
    feeder.join();

    const int ticks = g_ticks.load();
    const uint64_t proc = g_processed.load();
    std::printf("thread_tick_test: ticks=%d processed=%llu (expect ticks>=10, processed>=100)\n",
                ticks, static_cast<unsigned long long>(proc));
    // 1500ms / 100ms = 15 个节拍，放宽到 >=10（调度噪声裕量）；
    // 300 条消息应处理绝大部分（单条处理微秒级）
    const bool ok = ticks >= 10 && proc >= 100;
    return ok ? 0 : 1;
}
