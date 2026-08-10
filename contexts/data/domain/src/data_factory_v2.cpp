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

// 从 raw 帧取测量对象 hash 键（cell: cellId+cpId / ue: cellId+ueId）。
// 约定：rawData = dataType(u16) + cellId(u32) + cpId/ueId(u32) + 数据字段
// 性能测计数（ForEach 调用 = 切分次数；Flush 调用 = publish 次数；skip = Process 提前返回）
uint64_t g_forEachTotal = 0;
uint64_t g_flushTotal = 0;
uint64_t g_skipSink = 0;  // m_sink==null 跳过数
uint64_t g_skipTask = 0;  // m_currentTask==0 跳过数

uint64_t ExtractKey(uint16_t dataType, const void* rawData, uint32_t len) {
    if (len < sizeof(uint16_t) + sizeof(uint32_t) * 2) {
        return 0;  // 帧不足，占位键
    }
    const auto* p = static_cast<const uint32_t*>(rawData);
    if (dataType == static_cast<uint16_t>(DataType::CELL)) {
        return CellHashKey(p[1], p[2]);  // 跳过 dataType，取 cellId/cpId
    }
    return UeHashKey(p[1], p[2]);
}

}  // namespace

DataFactory& DataFactory::Instance() {
    static DataFactory inst;
    return inst;
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

    auto& reg = ExtractorRegistry::Instance();
    auto& mem = DataMemManager::Instance();
    const uint64_t key = ExtractKey(dataType, rawData, len);
    const uint64_t ts = static_cast<uint64_t>(m_tick) * kTickMs;

    // 合并上报：遍历 extractor 切分 → 攒到 aggregator → 帧末一次 publish（替代每 dataId 一发）
    // 优化：同一测量对象的 500 dataId 共享一个 block，AcquireBlock 一次，SlotOf 各槽（省 499 次 hash 探测）
    CacheHead* block = mem.AcquireBlock(dataType, kPeriod1S, key, m_tick);
    m_agg.Begin(m_currentTask);
    reg.ForEachByDomain(dataType, [&](const ExtractorSpec& spec) {
        if (spec.proc == nullptr) {
            return;
        }
        void* cache = nullptr;
        if (spec.needCache) {
            cache = (block != nullptr) ? mem.SlotOf(block, spec.dataId) : nullptr;
        } else {
            cache = mem.FixedSlot(spec.dataId);
        }
        if (cache == nullptr) return;
        spec.proc->Extra(rawData, len, cache);
        spec.proc->Hton(cache, spec.cacheSize);
        m_agg.Add(spec.dataId, cache, spec.cacheSize);  // 攒切片
        ++g_forEachTotal;
    });
    if (GetBypass() == Bypass::PROCESS) {
        return;  // 隔离测：切分不发，量切分 CPU 极限（无 publish 开销）
    }
    m_agg.Flush(ts, ++m_seq);  // 一次 publish：500 dataId 合并一帧 → sink → web/网管
    ++g_flushTotal;
}

void DataFactory::OnTick() {
    ++m_tick;
    DataMemManager::Instance().EvictExpired(m_tick);  // TTL：超周期槽位回收
}

}  // namespace dts::data
