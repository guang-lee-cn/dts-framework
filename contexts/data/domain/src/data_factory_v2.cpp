#include "data_factory_v2.h"

#include "data_config.h"
#include "data_ids.h"
#include "data_mem_manager.h"
#include "extractor_registry.h"

namespace dts::data {

DataFactory& DataFactory::Instance() {
    static DataFactory inst;
    return inst;
}

void DataFactory::Process(const void* rawData, uint32_t len, uint16_t dataType, uint32_t nowTick) {
    m_tick = nowTick;
    auto& reg = ExtractorRegistry::Instance();
    auto& mem = DataMemManager::Instance();

    // 测量对象 hash 键（从 DataHeader 提取；阶段 6 mock 注入完整 DataHeader 后再取真实字段）
    // 当前 MVP：rawData 首 sizeof(DataHeader) 作为 header 占位，键从 header 取
    if (len < sizeof(uint16_t)) {
        return;  // 连 dataType 都放不下
    }

    // 遍历该 dataType 下所有注册加工类（阶段 4 接 TdMap 后改为"激活集"过滤）
    reg.ForEachByDomain(dataType, [&](const ExtractorSpec& spec) {
        // 取测量对象键（cell: cellId+cpId / ue: cellId+ueId）。MVP 阶段从 rawData 头部约定字段取
        // 此处先用占位键 0（阶段 6 mock 提供真实 DataHeader 后补全）
        uint64_t key = 0;
        void* cache = nullptr;
        if (spec.needCache) {
            CacheHead* block = mem.AcquireBlock(dataType, spec.periodTicks, key, nowTick);
            if (block == nullptr) return;
            cache = mem.SlotOf(block, spec.dataId);
        } else {
            cache = mem.FixedSlot(spec.dataId);
        }
        if (cache == nullptr || spec.proc == nullptr) return;

        // 阶段一：Extra 加工入槽
        spec.proc->Extra(rawData, cache);
        spec.proc->Hton(cache, spec.cacheSize);

        // 阶段二：周期到达则 Report（阶段 4 接 TdMap 上报过滤后分发 task 上报缓存）
        // MVP：周期判定 nowTick % periodTicks == 0
        if (nowTick % spec.periodTicks == 0) {
            // TODO(阶段4): 按 TdMap 过滤 → 跟踪该 (dataId, key) 的 taskId 集 → 分发各自 ReportBuf
        }
    });
}

void DataFactory::FlushReports() {
    // 阶段 5：DataMemManager::ForEachReport → publish_external(REPORT) → 清空 used
    // 阶段 3 暂留桩，定时器接入后实现
}

}  // namespace dts::data
