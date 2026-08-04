#pragma once

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "dts_agent/dts_agent_api.h"

namespace dts {

// 模拟 SPA 进程：业务写乒乓（2ms，帧带 dataType）+ dts-agent 独立库联编（读翻转 1s -> itran pub）
class SpaMock {
public:
    ~SpaMock();

    void Start();

private:
    void writerLoop();
    static void buildCellFrame(std::vector<uint8_t>& out, uint16_t cellId, uint32_t prb);
    static void buildUeFrame(std::vector<uint8_t>& out, uint16_t ueId, uint16_t bler);

    PingPongBuffer m_pingpong;
    AgentHandle m_agent = nullptr;
    std::thread m_writer;
    std::atomic<bool> m_stop{false};
};

}  // namespace dts
