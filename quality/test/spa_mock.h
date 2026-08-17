#pragma once

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "dts_agent/dts_agent_api.h"

namespace dts {

// 模拟 SPA 进程：业务写乒乓（2ms，新布局 raw 帧）+ dts-agent 独立库联编
// （读翻转 1s -> AgentPublish -> data 线程 msg3）+ 握手（TaskRequest -> data 建任务）
class SpaMock {
public:
    ~SpaMock();

    void Start();
    // 显式停止：停 writer + join agent 读线程。Shutdown 日志前必须先调（agent 是日志调用方）
    void Stop();

private:
    void writerLoop();

    PingPongBuffer m_pingpong;
    AgentHandle m_agent = nullptr;
    std::thread m_writer;
    std::atomic<bool> m_stop{false};
};

}  // namespace dts
