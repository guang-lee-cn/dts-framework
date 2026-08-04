#pragma once

#include "task_manager.h"

namespace dts {

// 策略基类：任务激活后的行为可替换
class TaskStrategy {
public:
    virtual ~TaskStrategy() = default;
    virtual void OnTaskCreated(const TaskConfig& cfg) = 0;
};

// 默认策略：打印任务登记信息
class DefaultTaskStrategy : public TaskStrategy {
public:
    void OnTaskCreated(const TaskConfig& cfg) override;
};

}  // namespace dts
