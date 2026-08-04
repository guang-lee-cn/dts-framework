#pragma once

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace dts {

constexpr uint32_t PER_TASK_CACHE_SIZE = 32 * 1024;

struct ReportHeader {
    uint16_t taskId;
    uint64_t timestampMs;
    uint32_t seq;
    uint32_t payloadLen;
};

struct ReportBlock {
    ReportHeader header;
    std::vector<uint8_t> payload;
};

// 每任务独立上报缓存，1s 定时取走。无锁：data 线程独占。
class ReportCache {
public:
    void Append(uint16_t taskId, const uint8_t* data, uint32_t len);
    std::vector<ReportBlock> TakeAll();
    size_t BlockCount() const;

private:
    struct Entry {
        uint32_t used = 0;
        std::array<uint8_t, PER_TASK_CACHE_SIZE> buf{};
        uint64_t ts = 0;
    };

    std::unordered_map<uint16_t, Entry> m_entries;
    uint32_t m_seq = 0;
};

}  // namespace dts
