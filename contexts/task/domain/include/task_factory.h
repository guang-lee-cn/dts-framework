#pragma once

#include "task_manager.h"

namespace dts {

// 简单工厂：按任务类型创建任务（扩展点：按 cfg.type 实例化 Task 子类）
class TaskFactory {
public:
    bool Create(TaskManager& mgr, const TaskConfig& cfg);
};

}  // namespace dts
