#pragma once

#include <cstdint>
#include <vector>

namespace dts::data {

// 上报出口抽象（domain 零技术依赖，不引 detmw 头；bootstrap 注入 MwPort 适配实现）
class ReportSink {
public:
    virtual ~ReportSink() = default;
    // 推送一帧上报数据（总头 + n×(子头+data)）
    virtual void Publish(const void* data, uint32_t len) = 0;
};

// 数据工厂：rawData 帧批处理。
// 流程：dataType 路由 → 激活集（该 dataType 下被跟踪的 dataId 并集）
//      → 命中加工类 → Extra 全加工入槽 → 周期到 → Report 分发 task 上报缓存
class DataFactory {
public:
    static DataFactory& Instance();

    // 任务消息（task 线程经 mailbox 投递）：data 线程内更新 TdMapV2
    void OnTask(uint32_t taskId, const std::vector<uint16_t>& dataIds, uint32_t periodMs);

    // 处理一帧 rawData（data 线程独占）。周期判定/TTL 用内部 m_tick（OnTick 推进，唯一时间源）
    void Process(const void* rawData, uint32_t len, uint16_t dataType);

    // 定时 tick（ThreadRun 100ms 超时邮件驱动）：推进 m_tick，触发 EvictExpired + FlushReports
    void OnTick();
    // 1s 推送各 task 上报缓存 → ReportSink，清空 used（OnTick 周期到调）
    void FlushReports();

    // 注入上报出口（bootstrap 装配 MwPort 适配）
    void SetReportSink(ReportSink* sink) { m_sink = sink; }

private:
    DataFactory() = default;
    uint32_t m_tick = 0;     // tick 计数（1 tick = 100ms）
    ReportSink* m_sink = nullptr;
};

}  // namespace dts::data
