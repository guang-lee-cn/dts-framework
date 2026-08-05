#include "task_strategy.h"

#include "log.h"

namespace dts {

void DefaultTaskStrategy::OnTaskCreated(const TaskConfig& cfg) {
    log::Info("[task:strategy] task {} type={} created, tracking {} dataIds", cfg.taskId,
              cfg.type, cfg.dataIds.size());
}

}  // namespace dts
