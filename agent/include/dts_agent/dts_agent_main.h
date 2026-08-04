#pragma once

#include "dts_agent/dts_agent_api.h"

namespace dts {

// agent 上电入口（SPA 进程内调用）：创建读乒乓线程
inline AgentHandle dts_agent_main(const AgentCfg* cfg) {
    return dts_agent_create(cfg);
}

}  // namespace dts
