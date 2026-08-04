#include "tdmap.h"

namespace dts {

void TdMap::Track(uint16_t taskId, const std::vector<uint16_t>& dataIds) {
    auto& ds = m_taskToData[taskId];
    for (uint16_t id : dataIds) {
        ds.insert(id);
        m_dataToTask[id].insert(taskId);
    }
}

void TdMap::Untrack(uint16_t taskId) {
    auto it = m_taskToData.find(taskId);
    if (it == m_taskToData.end()) return;
    for (uint16_t id : it->second) {
        auto& ts = m_dataToTask[id];
        ts.erase(taskId);
        if (ts.empty()) {
            m_dataToTask.erase(id);
        }
    }
    m_taskToData.erase(it);
}

std::vector<uint16_t> TdMap::TasksOf(uint16_t dataId) const {
    auto it = m_dataToTask.find(dataId);
    if (it == m_dataToTask.end()) return {};
    return {it->second.begin(), it->second.end()};
}

std::vector<uint16_t> TdMap::DataIdsOf(uint16_t taskId) const {
    auto it = m_taskToData.find(taskId);
    if (it == m_taskToData.end()) return {};
    return {it->second.begin(), it->second.end()};
}

bool TdMap::IsTracked(uint16_t dataId) const {
    return m_dataToTask.count(dataId) != 0;
}

}  // namespace dts
