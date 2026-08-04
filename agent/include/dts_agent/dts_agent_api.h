#pragma once

#include <cstdint>

#include "dts_agent/pingpong.h"

namespace dts {

// 发布回调：SPA 进程注入，包装其 itran 发布通道
using AgentPubFn = void (*)(uint64_t key, const void* data, uint32_t len, void* ctx);

struct AgentCfg {
    PingPongBuffer* pingpong;   // SPA 与 agent 共享的乒乓 buffer
    AgentPubFn publish;         // 发布回调
    void* pubCtx;               // 发布回调上下文
    uint32_t readPeriodMs;      // 乒乓读周期（1s = 1000）
    uint64_t outKey;            // 数据发布 key
};

struct AgentImpl;
using AgentHandle = AgentImpl*;

// 创建并启动 agent（内部起读线程）
AgentHandle dts_agent_create(const AgentCfg* cfg);
// 停止并销毁
void dts_agent_destroy(AgentHandle h);

}  // namespace dts
