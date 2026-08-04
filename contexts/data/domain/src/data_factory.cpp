#include "data_factory.h"

#include <cstring>

#include "report_cache.h"
#include "tdmap.h"

namespace dts {

DataFactory& DataFactory::Instance() {
    static DataFactory inst;
    return inst;
}

void DataFactory::RegisterProcessor(uint16_t dataId, uint16_t dataType,
                                    std::unique_ptr<DataConstruct> proc,
                                    uint32_t tempOffset, uint32_t structSize) {
    // 校验临时缓存边界（无锁：注册发生在 data 线程上下文）
    if (tempOffset + structSize > TEMP_BLOCK_SIZE || m_procs.count(dataId) != 0) return;

    uint16_t slot = static_cast<uint16_t>(m_procs.size());
    if (slot >= MAX_DATA_IDS) return;

    proc->BindTempCache(m_tempPool[slot].data() + tempOffset, structSize);
    m_typeToDataIds[dataType].push_back(dataId);
    m_procs[dataId] = std::move(proc);
}

const BigFrameHeader* DataFactory::ParseHeader(char* data, uint32_t len) const {
    if (data == nullptr || len < sizeof(BigFrameHeader)) return nullptr;
    const auto* hdr = reinterpret_cast<const BigFrameHeader*>(data);
    if (hdr->magic != BIG_FRAME_MAGIC) return nullptr;
    return hdr;
}

DataFeedStat DataFactory::Feed(char* data, uint32_t len, const TdMap& tdmap, ReportCache& report) {
    DataFeedStat stat;
    const auto* hdr = ParseHeader(data, len);
    if (hdr == nullptr || hdr->dataCount == 0 || hdr->dataCount > 32) return stat;
    if (hdr->dataCount > (len - sizeof(BigFrameHeader)) / sizeof(BigFrameHeader::Entry)) return stat;

    // 按 DataType 匹配所有 dataId（骨架：只匹配单个头，帧流拼接后续补）
    auto it = m_typeToDataIds.find(hdr->dataType);
    if (it == m_typeToDataIds.end()) return stat;

    for (uint16_t dataId : it->second) {
        ++stat.matchedDataIds;
        auto tasks = tdmap.TasksOf(dataId);   // 无锁，data 线程独占
        if (tasks.empty()) {
            ++stat.dropped;
            continue;
        }
        // 从索引表定位该 dataId 的数据块
        const BigFrameHeader::Entry* entry = nullptr;
        for (uint32_t i = 0; i < hdr->dataCount; ++i) {
            if (hdr->entries[i].dataId == dataId) {
                entry = &hdr->entries[i];
                break;
            }
        }
        if (entry == nullptr || entry->offset + entry->len > len) {
            ++stat.dropped;
            continue;
        }
        auto pit = m_procs.find(dataId);
        if (pit == m_procs.end()) {
            ++stat.dropped;
            continue;
        }
        pit->second->BindReportCache(&report);
        pit->second->Process(data + entry->offset, entry->len, tasks);
        stat.tracked += 1;
        stat.cached += tasks.size();
    }
    return stat;
}

}  // namespace dts

// 领域层初始化（全局）：注册加工子类，data 线程下任务时调用
void DataDomainInit() {
    RegisterDataId_DATA_ID_CELL_PRB();
    RegisterDataId_DATA_ID_UE_BLER();
}