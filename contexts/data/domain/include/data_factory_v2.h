#pragma once

#include <cstdint>
#include <vector>

#include "report_aggregator.h"

namespace dts::data {

// 上报出口抽象（domain 零技术依赖，不引 detmw 头；bootstrap 注入 DtsMw 适配实现）
class ReportSink {
public:
    virtual ~ReportSink() = default;
    // 推送一帧上报数据（ReportHeader + n×(SubHeader+data)）
    virtual void Publish(const void* data, uint32_t len) = 0;
};

// 显式调用：强制 extractors.o 链接（静态注册对象随 .o 初始化）。bootstrap 装配时调
void InitExtractors();

// 数据工厂（直通模式）：rawData → dataType 路由 → 遍历该 dataType 全部注册 extractor
//      → Extra 切 → ReportAggregator 攒 → 帧末一次 Flush（经 ReportSink）。
// 握手标记 currentTask；每帧 1 次 publish（合并，替代每 dataId 一发）。
class DataFactory {
public:
    static DataFactory& Instance();

    // 握手任务消息（spaMock 经 mailbox 投递）：标记 currentTask（直通模式单 task）
    void OnTask(uint32_t taskId, const std::vector<uint16_t>& dataIds, uint32_t periodMs);

    // 处理一帧 rawData（data 线程独占）：Extra 切 → 攒 → 帧末一次 publish。TTL 用 m_tick
    void Process(const void* rawData, uint32_t len, uint16_t dataType);

    // 定时 tick（ThreadRun 100ms 超时邮件驱动）：推进 m_tick + EvictExpired
    void OnTick();

    // 注入上报出口（bootstrap 装配 DtsMw 适配）
    void SetReportSink(ReportSink* sink) { m_sink = sink; m_agg.SetSink(sink); }

private:
    DataFactory() = default;
    uint32_t m_tick = 0;          // tick 计数（1 tick = 100ms）
    uint32_t m_currentTask = 0;   // 当前握手任务（Report 填 taskId）
    uint32_t m_seq = 0;           // 上报序号（Report 填 seq，递增）
    ReportSink* m_sink = nullptr;
    ReportAggregator m_agg;       // 上报聚合（每帧攒 500 dataId，Flush 一次 publish）
};

}  // namespace dts::data
