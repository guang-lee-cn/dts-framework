#include "task_manager.h"

namespace dts {

TaskManager& TaskManager::Instance() {
    static TaskManager mgr;
    return mgr;
}

bool TaskManager::Add(const TaskConfig& cfg) {
    if (m_tasks.size() >= kMaxTasks || m_tasks.count(cfg.taskId) != 0) return false;
    m_tasks.emplace(cfg.taskId, cfg);
    return true;
}

bool TaskManager::Remove(uint16_t taskId) {
    return m_tasks.erase(taskId) != 0;
}

size_t TaskManager::Size() const {
    return m_tasks.size();
}

const TaskConfig* TaskManager::Find(uint16_t taskId) const {
    auto it = m_tasks.find(taskId);
    return it == m_tasks.end() ? nullptr : &it->second;
}

}  // namespace dts
