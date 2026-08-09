#include "data_factory_v2.h"

#include <cstring>

#include "data_config.h"
#include "data_ids.h"
#include "data_mem_manager.h"
#include "extractor_registry.h"
#include "tdmap_v2.h"

namespace dts::data {

namespace {

// data 线程领域对象（线程内独占，无锁）
TdMapV2 g_tdmap;

// 从 raw 帧首部取测量对象 hash 键（cell: cellId+cpId / ue: cellId+ueId）。
// 约定：rawData = DataHeader{dataType} + 测量对象键字段（阶段 6 mock 提供完整 DataHeader）
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

// data 线程消息处理：任务消息 → TdMapV2 增删改
void DataFactory::OnTask(uint32_t taskId, const std::vector<uint16_t>& dataIds, uint32_t periodMs) {
    if (dataIds.empty()) {
        g_tdmap.RemoveTask(taskId);  // 空 dataId 视为删任务
        return;
    }
    TaskInfo t;
    t.taskId = taskId;
    t.dataIds = dataIds;
    t.periodMs = periodMs;
    g_tdmap.UpsertTask(t);
}

void DataFactory::Process(const void* rawData, uint32_t len, uint16_t dataType) {
    if (g_tdmap.Empty()) {
        return;  // 无任务跟踪，跳过（工厂零加工）
    }

    auto& reg = ExtractorRegistry::Instance();
    auto& mem = DataMemManager::Instance();
    const uint64_t key = ExtractKey(dataType, rawData, len);

    // 激活集：该 dataType 下被跟踪的 dataId 并集（替代全集遍历）
    const std::vector<uint16_t> active = g_tdmap.ActiveDataIds(dataType);
    for (uint16_t dataId : active) {
        const ExtractorSpec* spec = reg.Find(dataType, dataId);
        if (spec == nullptr || spec->proc == nullptr) {
            continue;
        }

        // 阶段一：定位槽 → Extra 加工 → Hton（TTL 用 m_tick）
        void* cache = nullptr;
        if (spec->needCache) {
            CacheHead* block = mem.AcquireBlock(dataType, spec->periodTicks, key, m_tick);
            if (block == nullptr) continue;
            cache = mem.SlotOf(block, dataId);
        } else {
            cache = mem.FixedSlot(dataId);
        }
        if (cache == nullptr) continue;
        spec->proc->Extra(rawData, cache);
        spec->proc->Hton(cache, spec->cacheSize);

        // 阶段二：周期到达 → Report 分发到跟踪该 dataId 的各 task 上报缓存
        if (m_tick % spec->periodTicks != 0) {
            continue;  // 周期未到，等下一帧
        }
        const std::vector<uint32_t> tasks = g_tdmap.TasksOfDataId(dataId);
        for (uint32_t taskId : tasks) {
            ReportBuf* rb = mem.AcquireReport(taskId);
            if (rb == nullptr) continue;
            if (rb->used == 0) {
                rb->header.taskId = taskId;
                rb->header.timestampMs = static_cast<uint64_t>(m_tick) * kTickMs;
            }
            // 子头 + data 追加（基类 Report 默认实现，dst = payload 当前写入位）
            uint8_t* dst = rb->payload + rb->used;
            uint32_t cap = ReportBuf::kCap - rb->used;
            // 子头 dataId 填充（基类 Report 默认不填 dataId）
            SubHeader* sh = reinterpret_cast<SubHeader*>(dst);
            const int n = spec->proc->Report(cache, spec->cacheSize, dst, cap);
            if (n < 0) {
                break;  // 超容（≤32k）：剩余丢弃（策略待定，阶段 7）
            }
            sh->dataId = dataId;
            rb->used += static_cast<uint32_t>(n);
        }
    }
}

void DataFactory::OnTick() {
    ++m_tick;
    auto& mem = DataMemManager::Instance();
    mem.EvictExpired(m_tick);   // TTL：超周期槽数据失效归还

    // 1s（10 tick）推送上报缓存
    if (m_tick % (1000 / kTickMs) != 0) {
        return;
    }
    FlushReports();
}

void DataFactory::FlushReports() {
    if (m_sink == nullptr) {
        return;  // 无上报出口（未装配），跳过
    }
    auto& mem = DataMemManager::Instance();
    mem.ForEachReport([this](ReportBuf& rb) {
        if (rb.used == 0) {
            return;  // 本周期无数据
        }
        rb.header.payloadLen = rb.used;
        m_sink->Publish(&rb, sizeof(ReportHeader) + rb.used);  // 总头 + payload
        rb.used = 0;  // 清空，下周期重新累积
    });
}

}  // namespace dts::data
