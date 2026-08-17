#include "data_factory_v2.h"

#include <chrono>
#include <cstdlib>
#include <cstring>

#include "data_config.h"
#include "data_mem_manager.h"
#include "extractor_registry.h"
#include "log.h"

namespace dts::data {

namespace {

// 性能隔离测探：DETMW_DATA_BYPASS=recv（只收不切，量 DDS 收极限）/
//   process（切分不发，量切分 CPU 极限）/ 默认 full（完整链路）
enum class Bypass { FULL, RECV, PROCESS };
Bypass GetBypass() {
    static const Bypass m = [] {
        const char* e = std::getenv("DETMW_DATA_BYPASS");
        if (e != nullptr && std::strcmp(e, "recv") == 0) return Bypass::RECV;
        if (e != nullptr && std::strcmp(e, "process") == 0) return Bypass::PROCESS;
        return Bypass::FULL;
    }();
    return m;
}

// 性能测计数（ForEach 调用 = 切分次数；Flush 调用 = publish 次数；skip = Process 提前返回）
uint64_t g_forEachTotal = 0;
uint64_t g_flushTotal = 0;
uint64_t g_skipSink = 0;  // m_sink==null 跳过数
uint64_t g_skipTask = 0;  // m_currentTask==0 跳过数

}  // namespace

// 从 raw 帧取测量对象 hash 键（cell: cellId+cpId / ue: cellId+ueId）。
// 约定：rawData = dataType(u16) + cellId(u32) + cpId/ueId(u32) + 数据字段
// 注意：按字节偏移 memcpy 解析（布局可非对齐；曾用 uint32_t* 错位读取导致所有小区
// hash 键错乱、共享同一 cache block —— 商用前必修，见 worklog 2026-08-17）
uint64_t ExtractRawKey(uint16_t dataType, const void* rawData, uint32_t len) {
    if (rawData == nullptr || len < sizeof(uint16_t) + sizeof(uint32_t) * 2) {
        return 0;  // 帧不足，占位键
    }
    const auto* b = static_cast<const uint8_t*>(rawData);
    uint32_t cellId = 0;
    uint32_t second = 0;  // cpId（CELL）/ ueId（UE）
    std::memcpy(&cellId, b + sizeof(uint16_t), sizeof(cellId));
    std::memcpy(&second, b + sizeof(uint16_t) + sizeof(uint32_t), sizeof(second));
    if (dataType == static_cast<uint16_t>(DataType::CELL)) {
        return CellHashKey(cellId, second);
    }
    return UeHashKey(cellId, second);
}

DataFactory& DataFactory::Instance() {
    static DataFactory inst;
    return inst;
}

// 预算阈值：环境变量 DTS_DATA_BUDGET_WARN_US / DTS_DATA_BUDGET_ALARM_US 覆盖默认
// （部署按硬件档位定；运行时 console set_data_budget 可再调）
DataFactory::DataFactory() {
    if (const char* v = std::getenv("DTS_DATA_BUDGET_WARN_US")) {
        m_budgetWarnUs.store(std::strtoull(v, nullptr, 10), std::memory_order_relaxed);
    }
    if (const char* v = std::getenv("DTS_DATA_BUDGET_ALARM_US")) {
        m_budgetAlarmUs.store(std::strtoull(v, nullptr, 10), std::memory_order_relaxed);
    }
}

int DataFactory::SetBudget(uint64_t warnUs, uint64_t alarmUs) {
    if (warnUs == 0 || alarmUs == 0 || warnUs > alarmUs) {
        return -1;  // 非法：warn 必须 ≤ alarm 且均 > 0
    }
    m_budgetWarnUs.store(warnUs, std::memory_order_relaxed);
    m_budgetAlarmUs.store(alarmUs, std::memory_order_relaxed);
    return 0;
}

DataStatsSnapshot DataFactory::Stats() const {
    DataStatsSnapshot s;
    s.frames = m_stats.frames.load(std::memory_order_relaxed);
    s.frameMaxUs = m_stats.frameMaxUs.load(std::memory_order_relaxed);
    s.frameBudgetWarn = m_stats.frameBudgetWarn.load(std::memory_order_relaxed);
    s.frameBudgetAlarm = m_stats.frameBudgetAlarm.load(std::memory_order_relaxed);
    s.dropPoolFull = m_stats.dropPoolFull.load(std::memory_order_relaxed);
    s.dropSliceOverCap = m_stats.dropSliceOverCap.load(std::memory_order_relaxed);
    s.keyFail = m_stats.keyFail.load(std::memory_order_relaxed);
    s.ticks = m_stats.ticks.load(std::memory_order_relaxed);
    s.budgetWarnUs = m_budgetWarnUs.load(std::memory_order_relaxed);
    s.budgetAlarmUs = m_budgetAlarmUs.load(std::memory_order_relaxed);
    // 聚合组件超容计数并入（data 线程独占写，relaxed 读足够）
    s.dropSliceOverCap += m_agg.Drops();
    return s;
}

void DataFactory::OnTask(uint32_t taskId, const std::vector<uint16_t>& /*dataIds*/,
                         uint32_t /*periodMs*/) {
    // 直通模式：握手标记当前 task（单 task），切分直推时 Report 填 ReportHeader.taskId
    m_currentTask = taskId;
    dts::log::Info("[data] OnTask currentTask={}", taskId);
}

void DataFactory::Process(const void* rawData, uint32_t len, uint16_t dataType) {
    ++m_procCount;  // 性能测：Process 调用计数（含 bypass 模式，量 data 消费速率）
    // wall-clock consume rate（每 1s log；不依赖 OnTick，避免 mailbox 拥挤时 timer 排队延迟污染）
    static auto s_lastLog = std::chrono::steady_clock::now();
    static uint64_t s_lastCount = 0;
    const auto now = std::chrono::steady_clock::now();
    if (now - s_lastLog >= std::chrono::seconds(1)) {
        static uint64_t s_lastForEach = 0;
        static uint64_t s_lastFlush = 0;
        static uint64_t s_lastSinkSkip = 0;
        static uint64_t s_lastTaskSkip = 0;
        dts::log::Info("[data:perf] consume={} forEach={} flush={} skipSink={} skipTask={}",
                       m_procCount - s_lastCount, g_forEachTotal - s_lastForEach,
                       g_flushTotal - s_lastFlush, g_skipSink - s_lastSinkSkip,
                       g_skipTask - s_lastTaskSkip);
        s_lastCount = m_procCount;
        s_lastForEach = g_forEachTotal;
        s_lastFlush = g_flushTotal;
        s_lastSinkSkip = g_skipSink;
        s_lastTaskSkip = g_skipTask;
        s_lastLog = now;
    }
    if (GetBypass() == Bypass::RECV) {
        return;  // 隔离测：只收不切，量 DDS 收极限（data reader → mailbox → Process 退出）
    }
    if (m_sink == nullptr) {
        ++g_skipSink;
        return;
    }
    if (m_currentTask == 0) {
        ++g_skipTask;
        return;
    }

    // ---- 单帧处理预算打点（口径：线程内处理含业务代码；超限计数告警，不静默）----
    const auto t0 = std::chrono::steady_clock::now();

    auto& reg = ExtractorRegistry::Instance();
    auto& mem = DataMemManager::Instance();
    const uint64_t key = ExtractRawKey(dataType, rawData, len);
    if (key == 0) {
        m_stats.keyFail.fetch_add(1, std::memory_order_relaxed);  // 帧头解析失败（占位键）
    }
    const uint64_t ts = static_cast<uint64_t>(m_tick) * kTickMs;

    // 合并上报：遍历 extractor 切分 → 攒到 aggregator → 帧末一次 publish（替代每 dataId 一发）
    // 优化：同一测量对象的 500 dataId 共享一个 block，AcquireBlock 一次，SlotOf 各槽（省 499 次 hash 探测）
    CacheHead* block = mem.AcquireBlock(dataType, kPeriod1S, key, m_tick);
    if (block == nullptr) {
        m_stats.dropPoolFull.fetch_add(1, std::memory_order_relaxed);  // 池满：整帧丢弃，计数
        return;
    }
    m_agg.Begin(m_currentTask);
    reg.ForEachByDomain(dataType, [&](const ExtractorSpec& spec) {
        if (spec.proc == nullptr) {
            return;
        }
        void* cache = nullptr;
        if (spec.needCache) {
            cache = mem.SlotOf(block, spec.dataId);
        } else {
            cache = mem.FixedSlot(spec.dataId);
        }
        if (cache == nullptr) return;
        if (spec.proc->Extra(rawData, len, cache) < 0) {
            return;  // 切片越界（帧比切片偏移短，如小帧/截断）：跳过，不进上报
        }
        spec.proc->Hton(cache, spec.cacheSize);
        m_agg.Add(spec.dataId, cache, spec.cacheSize);  // 攒切片
        ++g_forEachTotal;
    });
    if (GetBypass() == Bypass::PROCESS) {
        return;  // 隔离测：切分不发，量切分 CPU 极限（无 publish 开销）
    }
    m_agg.Flush(ts, ++m_seq);  // 一次 publish：500 dataId 合并一帧 → sink → web/网管
    ++g_flushTotal;

    // ---- 预算核算：帧处理耗时（含切分 + 合并上报）----
    const auto t1 = std::chrono::steady_clock::now();
    const uint64_t us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
    m_stats.frames.fetch_add(1, std::memory_order_relaxed);
    uint64_t prevMax = m_stats.frameMaxUs.load(std::memory_order_relaxed);
    while (us > prevMax &&
           !m_stats.frameMaxUs.compare_exchange_weak(prevMax, us, std::memory_order_relaxed)) {
    }
    const uint64_t warnUs = m_budgetWarnUs.load(std::memory_order_relaxed);
    const uint64_t alarmUs = m_budgetAlarmUs.load(std::memory_order_relaxed);
    if (us >= alarmUs) {
        m_stats.frameBudgetAlarm.fetch_add(1, std::memory_order_relaxed);
        dts::log::Error("[data:budget] frame {}us >= {}us (determinism budget exceeded)", us,
                        alarmUs);
    } else if (us >= warnUs) {
        m_stats.frameBudgetWarn.fetch_add(1, std::memory_order_relaxed);
        dts::log::Warn("[data:budget] frame {}us >= {}us (approaching budget)", us, warnUs);
    }
}

void DataFactory::OnTick() {
    ++m_tick;
    m_stats.ticks.fetch_add(1, std::memory_order_relaxed);  // 维护节拍计数（TTL 回收驱动）
    DataMemManager::Instance().EvictExpired(m_tick);        // TTL：超周期槽位回收
    // 周期指标出口（P1-2）：每 10s 一条结构化日志（单行 key=value，日志采集/网管解析用；
    // 实时查询走 console get_data_stats / oam 业务组拉取）
    if (m_tick % 100 == 0) {
        const DataStatsSnapshot s = Stats();
        dts::log::Info("[data:stats] frames={} max_us={} warn={} alarm={} ticks={} "
                       "drop_pool_full={} drop_slice_overcap={} key_fail={} budget_warn_us={} "
                       "budget_alarm_us={}",
                       s.frames, s.frameMaxUs, s.frameBudgetWarn, s.frameBudgetAlarm, s.ticks,
                       s.dropPoolFull, s.dropSliceOverCap, s.keyFail, s.budgetWarnUs,
                       s.budgetAlarmUs);
    }
}

}  // namespace dts::data
