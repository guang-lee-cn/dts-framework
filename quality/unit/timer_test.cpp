// 单元自测：TimerWheel（时间轮：周期任务注册/取消/到期触发/漂移不累积）。
// 运行：构建后直接执行；退出码 0 = 全过。
#include <cstdio>

#include "timer.h"

using namespace dts;

namespace {

int g_checks = 0;
int g_fails = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        ++g_checks;                                                     \
        if (!(cond)) {                                                  \
            ++g_fails;                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                               \
    } while (0)

int g_fires = 0;
int g_lastTick = -1;

void OnFire(uint32_t nowTick) {
    ++g_fires;
    g_lastTick = static_cast<int>(nowTick);
}

void TestPeriodic() {
    g_fires = 0;
    g_lastTick = -1;
    TimerWheel tw;

    // 参数校验
    CHECK(tw.Add(0, OnFire) == -1);  // period 0 拒绝
    CHECK(tw.Add(10, nullptr) == -1);

    const int h = tw.Add(10, OnFire, 0);  // 10 tick 周期，now=0
    CHECK(h > 0);

    tw.Tick(5);   // 未到期
    CHECK(g_fires == 0);
    tw.Tick(10);  // 到期：now >= next(10)
    CHECK(g_fires == 1 && g_lastTick == 10);
    tw.Tick(15);  // 未到下次（next=20）
    CHECK(g_fires == 1);
    tw.Tick(25);  // 第二次到期（漂移后 next 从 25 起算，不按唤醒次数累积）
    CHECK(g_fires == 2);
    tw.Tick(35);
    CHECK(g_fires == 3);
}

void TestCancel() {
    g_fires = 0;
    TimerWheel tw;
    const int h = tw.Add(5, OnFire, 0);
    tw.Cancel(h);
    tw.Tick(5);
    tw.Tick(10);
    tw.Tick(15);
    CHECK(g_fires == 0);  // 取消后不再触发
}

}  // namespace

int main() {
    TestPeriodic();
    TestCancel();
    std::printf("timer_test: %d checks, %d fails\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
