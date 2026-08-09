#include "timer.h"

#include <algorithm>

namespace dts {

int TimerWheel::Add(uint32_t periodTicks, TickFn fn, uint32_t nowTick) {
    if (periodTicks == 0 || !fn) {
        return -1;
    }
    std::lock_guard<std::mutex> lk(m_mutex);
    Task t;
    t.handle = m_nextHandle++;
    t.period = periodTicks;
    t.next = nowTick + periodTicks;  // 相对 now 设到期，调度延迟不累积（漂移对策）
    t.fn = std::move(fn);
    t.active = true;
    m_tasks.push_back(std::move(t));
    return m_tasks.back().handle;
}

void TimerWheel::Cancel(int handle) {
    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto& t : m_tasks) {
        if (t.handle == handle) {
            t.active = false;
        }
    }
}

void TimerWheel::Tick(uint32_t nowTick) {
    std::vector<TickFn> fire;  // 锁外执行回调（不持锁回调，防卡死/重入）
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        for (auto& t : m_tasks) {
            if (!t.active) continue;
            // nowTick >= next：到期（单调 tick，next 每次加 period 不累积漂移）
            if (nowTick < t.next) continue;
            fire.push_back(t.fn);
            t.next = nowTick + t.period;  // 下次到期
        }
        // 清理已取消任务（惰性回收）
        m_tasks.erase(std::remove_if(m_tasks.begin(), m_tasks.end(),
                                     [](const Task& t) { return !t.active; }),
                      m_tasks.end());
    }
    for (const auto& fn : fire) {
        try {
            fn(nowTick);  // 卡死对策：回调不阻塞，异常不杀线程
        } catch (...) {
            // 吞异常（定时器不应因单任务异常停止）
        }
    }
}

}  // namespace dts
