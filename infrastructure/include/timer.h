#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace dts {

// 轻量时间轮（参考 Netty HashedWheelTimer）：单线程驱动，无跨线程开销。
// tick 由业务线程周期调用 Tick() 推进（漂移用 nowTick 绝对值，不按唤醒次数累积）。
// 回调约定不阻塞（重活另投 mailbox）；try/catch 防异常杀线程。
class TimerWheel {
public:
    using TickFn = std::function<void(uint32_t nowTick)>;

    // 注册周期任务，返回句柄（>0）；periodTicks = 0 或 fn 空返回 -1
    int Add(uint32_t periodTicks, TickFn fn, uint32_t nowTick = 0);
    void Cancel(int handle);

    // 推进到 nowTick，触发到期任务（periodTicks 的整数倍）。nowTick 单调不减
    void Tick(uint32_t nowTick);

private:
    struct Task {
        int handle = 0;
        uint32_t period = 0;
        uint32_t next = 0;
        TickFn fn;
        bool active = false;
    };
    mutable std::mutex m_mutex;
    std::vector<Task> m_tasks;
    int m_nextHandle = 1;
};

}  // namespace dts
