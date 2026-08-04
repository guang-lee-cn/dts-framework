#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace dts {

struct TaskConfig {
    uint16_t taskId;
    uint16_t type;
    std::vector<uint16_t> dataIds;
};

// 单例 TaskManager：任务生命周期管理，同类型任务最多 kMaxTasks 个。无锁：task 线程独占。
class TaskManager {
public:
    static constexpr uint16_t kMaxTasks = 64;
    static TaskManager& Instance();

    bool Add(const TaskConfig& cfg);
    bool Remove(uint16_t taskId);
    size_t Size() const;
    const TaskConfig* Find(uint16_t taskId) const;

private:
    TaskManager() = default;
    std::unordered_map<uint16_t, TaskConfig> m_tasks;
};

}  // namespace dts
