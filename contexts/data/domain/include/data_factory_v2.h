#pragma once

#include <atomic>
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

// 帧头解析：rawData = [u16 dataType][u32 cellId][u32 cpId/ueId][数据区]（按字节偏移，可非对齐）。
// 返回测量对象 hash 键（CELL → CellHashKey(cellId, cpId)；UE → UeHashKey(cellId, ueId)）。
// 帧不足返回 0（占位键，DataFactory 计 keyFail）。暴露供单测（对齐回归）。
uint64_t ExtractRawKey(uint16_t dataType, const void* rawData, uint32_t len);

// data 线程处理统计（原子：data 线程写，console/control 只读查询）。
// 口径：单帧处理耗时 = Process 进入 → 返回（含切分 + 合并上报，即"线程内处理含业务代码"）
struct DataStats {
    std::atomic<uint64_t> frames{0};            // 已处理帧数（Process 调用）
    std::atomic<uint64_t> frameMaxUs{0};        // 单帧处理耗时滚动最大（μs）
    std::atomic<uint64_t> frameBudgetWarn{0};   // 超预警阈值（3ms）次数
    std::atomic<uint64_t> frameBudgetAlarm{0};  // 超告警阈值（5ms = 确定性违约）次数
    std::atomic<uint64_t> dropPoolFull{0};      // 池满丢整帧（AcquireBlock 失败）
    std::atomic<uint64_t> dropSliceOverCap{0};  // 上报帧超容丢切片（ReportAggregator）
    std::atomic<uint64_t> keyFail{0};           // 帧头解析失败（键=0 占位）
    std::atomic<uint32_t> ticks{0};             // 维护节拍执行次数（OnTick，TTL 回收）
};

// 统计快照（普通值类型，控制面只读用；Stats() 原子读取构造）
struct DataStatsSnapshot {
    uint64_t frames = 0;
    uint64_t frameMaxUs = 0;
    uint64_t frameBudgetWarn = 0;
    uint64_t frameBudgetAlarm = 0;
    uint64_t dropPoolFull = 0;
    uint64_t dropSliceOverCap = 0;
    uint64_t keyFail = 0;
    uint32_t ticks = 0;
    uint64_t budgetWarnUs = 0;   // 当前预警阈值（可配置）
    uint64_t budgetAlarmUs = 0;  // 当前告警阈值（可配置）
};

// 数据工厂（直通模式）：rawData → dataType 路由 → 遍历该 dataType 全部注册 extractor
//      → Extra 切 → ReportAggregator 攒 → 帧末一次 Flush（经 ReportSink）。
// 握手标记 currentTask；每帧 1 次 publish（合并，替代每 dataId 一发）。
class DataFactory {
public:
    static DataFactory& Instance();

    // 握手任务消息（spaMock 经 mailbox 投递）：标记 currentTask（直通模式单 task）
    void OnTask(uint32_t taskId, const std::vector<uint16_t>& dataIds, uint32_t periodMs);

    // 处理一帧 rawData（data 线程独占）：Extra 切 → 攒 → 帧末一次 publish。TTL 用 m_tick。
    // 单帧处理耗时打点（预算告警，阈值可配置，见 SetBudget）
    void Process(const void* rawData, uint32_t len, uint16_t dataType);

    // 定时 tick（ThreadRun 100ms 绝对期限节拍）：推进 m_tick + EvictExpired
    void OnTick();

    // 注入上报出口（bootstrap 装配 DtsMw 适配）
    void SetReportSink(ReportSink* sink) { m_sink = sink; m_agg.SetSink(sink); }

    // 预算阈值（μs）：warn <= alarm 才生效（返回 0）；console set_data_budget 调用。
    // 默认/环境变量：DTS_DATA_BUDGET_WARN_US / DTS_DATA_BUDGET_ALARM_US（构造时读取）
    int SetBudget(uint64_t warnUs, uint64_t alarmUs);

    // 处理统计快照（原子读；console get_data_stats 用）
    DataStatsSnapshot Stats() const;

private:
    DataFactory();  // 构造读环境变量预算阈值
    uint32_t m_tick = 0;          // tick 计数（1 tick = 100ms）
    uint32_t m_currentTask = 0;   // 当前握手任务（Report 填 taskId）
    uint32_t m_seq = 0;           // 上报序号（Report 填 seq，递增）
    uint64_t m_procCount = 0;     // Process 调用计数（性能测：每秒 log 消费速率）
    uint64_t m_procSecBase = 0;   // 上一秒 Process 计数基线
    ReportSink* m_sink = nullptr;
    ReportAggregator m_agg;       // 上报聚合（每帧攒 500 dataId，Flush 一次 publish）
    DataStats m_stats;            // 处理统计（含预算告警 + 丢弃计数）
    std::atomic<uint64_t> m_budgetWarnUs{kFrameBudgetWarnUs};   // 预警阈值（默认 3ms）
    std::atomic<uint64_t> m_budgetAlarmUs{kFrameBudgetAlarmUs}; // 告警阈值（默认 5ms）
};

}  // namespace dts::data
