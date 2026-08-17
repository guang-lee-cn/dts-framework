#include "report_aggregator.h"

#include <cstring>

#include "data_factory_v2.h"  // ReportSink（domain 内部头，无环）

namespace dts::data {

void ReportAggregator::Add(uint16_t dataId, const void* cache, uint32_t len) {
    if (m_used + sizeof(SubHeader) + len > kCap) {
        m_drops.fetch_add(1, std::memory_order_relaxed);  // 超容丢切片：计数不静默
        return;  // kCap=48K：500 dataId 全量（34778B）装得下；dataId 扩容时重新核算
    }
    SubHeader sh{dataId, static_cast<uint16_t>(len)};
    std::memcpy(m_buf.data() + m_used, &sh, sizeof(sh));
    m_used += sizeof(sh);
    if (cache != nullptr && len > 0) {
        std::memcpy(m_buf.data() + m_used, cache, len);
        m_used += len;
    }
}

void ReportAggregator::Flush(uint64_t timestampMs, uint32_t seq) {
    if (m_sink == nullptr || m_used <= sizeof(ReportHeader)) {
        return;  // 无 sink 或空帧
    }
    auto* hdr = reinterpret_cast<ReportHeader*>(m_buf.data());
    hdr->taskId = m_taskId;
    hdr->timestampMs = timestampMs;
    hdr->seq = seq;
    hdr->payloadLen = m_used - sizeof(ReportHeader);
    m_sink->Publish(m_buf.data(), m_used);  // 一次 publish（换 sink = 换目标：web/网管）
    m_used = 0;
}

}  // namespace dts::data
