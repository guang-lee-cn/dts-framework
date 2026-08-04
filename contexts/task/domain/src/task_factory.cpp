#include "task_factory.h"

namespace dts {

bool TaskFactory::Create(TaskManager& mgr, const TaskConfig& cfg) {
    return mgr.Add(cfg);
}

}  // namespace dts
