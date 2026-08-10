#include "tdmap_v2.h"

#include "data_ids.h"

namespace dts::data {

void TdMapV2::UpsertTask(const TaskInfo& t) {
    m_tasks[t.taskId] = t;
    RebuildIndex();
}

void TdMapV2::RemoveTask(uint32_t taskId) {
    m_tasks.erase(taskId);
    RebuildIndex();
}

void TdMapV2::RebuildIndex() {
    m_dataToTasks.clear();
    for (const auto& kv : m_tasks) {
        for (uint16_t did : kv.second.dataIds) {
            m_dataToTasks[did].insert(kv.first);
        }
    }
}

std::vector<uint16_t> TdMapV2::ActiveDataIds(uint16_t dataType) const {
    std::unordered_set<uint16_t> seen;
    std::vector<uint16_t> out;
    for (const auto& kv : m_dataToTasks) {
        const uint16_t did = kv.first;
        const DataIdSpec s = SpecOf(did);
        if (s.dataId != 0 && s.dataType == dataType) {
            if (seen.insert(did).second) {
                out.push_back(did);
            }
        }
    }
    return out;
}

std::vector<uint32_t> TdMapV2::TasksOfDataId(uint16_t dataId) const {
    std::vector<uint32_t> out;
    auto it = m_dataToTasks.find(dataId);
    if (it != m_dataToTasks.end()) {
        out.assign(it->second.begin(), it->second.end());
    }
    return out;
}

}  // namespace dts::data
