#pragma once

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dts::data {

// 任务信息（task 线程经 mailbox 投递，data 线程独占消费）
struct TaskInfo {
    uint32_t taskId = 0;
    std::vector<uint16_t> dataIds;  // 跟踪的 dataId 列表
    uint32_t periodMs = 1000;       // 上报周期（ms）
};

// 任务信息库（data 线程独占，无锁——所有读写都在 data 线程内，task 经 mailbox 投递变更）。
// 职责：① 保存任务信息 ② 激活集（工厂每帧只处理被跟踪的 dataId 并集）③ 上报分发（dataId→taskId 集）
class TdMapV2 {
public:
    // 任务增/改（同 taskId 覆盖）/删
    void UpsertTask(const TaskInfo& t);
    void RemoveTask(uint32_t taskId);

    // 激活集：所有任务跟踪的 dataId 并集（按 dataType 过滤，工厂每帧用）
    std::vector<uint16_t> ActiveDataIds(uint16_t dataType) const;

    // 上报分发：跟踪某 dataId 的 taskId 集（Report 阶段，按 (dataId, 测量对象) 过滤）
    std::vector<uint32_t> TasksOfDataId(uint16_t dataId) const;

    bool Empty() const { return m_tasks.empty(); }

private:
    // 重建 dataId→taskId 反查表（任务增删后调）
    void RebuildIndex();

    std::unordered_map<uint32_t, TaskInfo> m_tasks;               // taskId → 任务信息
    std::unordered_map<uint16_t, std::unordered_set<uint32_t>> m_dataToTasks;  // dataId → taskId 集
};

}  // namespace dts::data
