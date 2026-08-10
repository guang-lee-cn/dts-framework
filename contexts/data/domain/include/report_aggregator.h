#pragma once

#include <array>
#include <cstdint>

#include "data_model.h"  // ReportHeader / SubHeader

namespace dts::data {

class ReportSink;  // domain 抽象，bootstrap 注入（web 适配/网管适配切换目标）

// 上报聚合组件：攒多个 dataId 切片到一帧，一次 publish（替代每 dataId 一发）。
// 换发送目标（webserverMock / 网管）= 换注入的 ReportSink，本组件零改动。
// data 线程独占，无锁。载荷：ReportHeader + n×(SubHeader + data)。
class ReportAggregator {
public:
    static constexpr uint32_t kCap = 32 * 1024;  // 单帧上报上限

    void SetSink(ReportSink* s) { m_sink = s; }
    // 新帧开始：记录 taskId，预留总头
    void Begin(uint32_t taskId) { m_taskId = taskId; m_used = sizeof(ReportHeader); }
    // 攒一个 dataId 切片（SubHeader + data 追加）。超容丢弃该切片（MVP 不分片）
    void Add(uint16_t dataId, const void* cache, uint32_t len);
    // 触发上报：填总头 → sink->Publish → 清空。空帧不发
    void Flush(uint64_t timestampMs, uint32_t seq);

private:
    ReportSink* m_sink = nullptr;
    uint32_t m_taskId = 0;
    uint32_t m_used = 0;  // m_buf 有效字节（含总头）
    std::array<uint8_t, kCap> m_buf{};
};

}  // namespace dts::data
