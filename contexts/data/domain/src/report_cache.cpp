#include "report_cache.h"

#include <chrono>
#include <cstring>
#include <utility>

namespace dts {

static uint64_t NowMs() {
    auto t = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(t).count();
}

void ReportCache::Append(uint16_t taskId, const uint8_t* data, uint32_t len) {
    if (len == 0 || len > PER_TASK_CACHE_SIZE) return;
    auto& e = m_entries[taskId];
    if (e.used + len > PER_TASK_CACHE_SIZE) return;  // 超 32k 丢弃
    std::memcpy(e.buf.data() + e.used, data, len);
    e.used += len;
    e.ts = NowMs();
}

std::vector<ReportBlock> ReportCache::TakeAll() {
    std::vector<ReportBlock> out;
    out.reserve(m_entries.size());
    for (auto& [taskId, e] : m_entries) {
        ReportBlock b;
        b.header.taskId = taskId;
        b.header.timestampMs = e.ts;
        b.header.seq = ++m_seq;
        b.header.payloadLen = e.used;
        b.payload.assign(e.buf.begin(), e.buf.begin() + e.used);
        out.push_back(std::move(b));
    }
    m_entries.clear();
    return out;
}

size_t ReportCache::BlockCount() const {
    return m_entries.size();
}

}  // namespace dts
