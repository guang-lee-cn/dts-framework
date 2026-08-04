#pragma once

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dts {

// task-data 映射：多对多，taskId <-> dataId 双向直查。无锁：data 线程独占。
class TdMap {
public:
    void Track(uint16_t taskId, const std::vector<uint16_t>& dataIds);
    void Untrack(uint16_t taskId);

    std::vector<uint16_t> TasksOf(uint16_t dataId) const;
    std::vector<uint16_t> DataIdsOf(uint16_t taskId) const;
    bool IsTracked(uint16_t dataId) const;

private:
    std::unordered_map<uint16_t, std::unordered_set<uint16_t>> m_taskToData;
    std::unordered_map<uint16_t, std::unordered_set<uint16_t>> m_dataToTask;
};

}  // namespace dts
