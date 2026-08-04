#include "task_strategy.h"

#include <cstdio>

namespace dts {

void DefaultTaskStrategy::OnTaskCreated(const TaskConfig& cfg) {
    std::printf("[task:strategy] task %u type=%u created, tracking %zu dataIds\n", cfg.taskId,
                cfg.type, cfg.dataIds.size());
}

}  // namespace dts
