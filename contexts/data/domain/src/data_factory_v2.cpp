#include "data_factory_v2.h"

#include "data_config.h"
#include "data_mem_manager.h"
#include "extractor_registry.h"

namespace dts::data {

namespace {

// 从 raw 帧取测量对象 hash 键（cell: cellId+cpId / ue: cellId+ueId）。
// 约定：rawData = dataType(u16) + cellId(u32) + cpId/ueId(u32) + 数据字段
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
}

void DataFactory::Process(const void* rawData, uint32_t len, uint16_t dataType) {
    if (m_sink == nullptr || m_currentTask == 0) {
        return;  // 无上报出口或未握手建任务，跳过
    }

    auto& reg = ExtractorRegistry::Instance();
    auto& mem = DataMemManager::Instance();
    const uint64_t key = ExtractKey(dataType, rawData, len);
    const uint64_t ts = static_cast<uint64_t>(m_tick) * kTickMs;

    // 直通：遍历该 dataType 全部注册 extractor，Extra 切 → Report 直推 webserver
    reg.ForEachByDomain(dataType, [&](const ExtractorSpec& spec) {
        if (spec.proc == nullptr) {
            return;
        }
        void* cache = nullptr;
        if (spec.needCache) {
            CacheHead* block = mem.AcquireBlock(dataType, spec.periodTicks, key, m_tick);
            if (block == nullptr) return;
            cache = mem.SlotOf(block, spec.dataId);
        } else {
            cache = mem.FixedSlot(spec.dataId);
        }
        if (cache == nullptr) return;
        spec.proc->Extra(rawData, len, cache);
        spec.proc->Hton(cache, spec.cacheSize);

        ReportCtx ctx{cache, spec.cacheSize, spec.dataId, m_currentTask, ts, ++m_seq, m_sink};
        spec.proc->Report(ctx);  // 基类直推：cache → sink → webserver
    });
}

void DataFactory::OnTick() {
    ++m_tick;
    DataMemManager::Instance().EvictExpired(m_tick);  // TTL：超周期槽位回收
}

}  // namespace dts::data
