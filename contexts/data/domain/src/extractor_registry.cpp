#include "extractor_registry.h"

namespace dts::data {

ExtractorRegistry& ExtractorRegistry::Instance() {
    static ExtractorRegistry inst;
    return inst;
}

bool ExtractorRegistry::Register(uint16_t dataId, uint16_t dataType, uint32_t cacheSize,
                                 uint32_t periodTicks, bool needCache,
                                 std::unique_ptr<Extractor> proc) {
    const uint32_t key = (static_cast<uint32_t>(dataType) << 16) | dataId;
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_table.find(key) != m_table.end()) {
        return false;  // 重名
    }
    auto spec = std::make_unique<ExtractorSpec>();
    spec->dataId = dataId;
    spec->dataType = dataType;
    spec->cacheSize = cacheSize;
    spec->periodTicks = periodTicks;
    spec->needCache = needCache;
    spec->proc = std::move(proc);
    m_table.emplace(key, std::move(spec));
    return true;
}

const ExtractorSpec* ExtractorRegistry::Find(uint16_t dataType, uint16_t dataId) const {
    const uint32_t key = (static_cast<uint32_t>(dataType) << 16) | dataId;
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_table.find(key);
    return it == m_table.end() ? nullptr : it->second.get();
}

void ExtractorRegistry::ForEachByDomain(uint16_t dataType,
                                        const std::function<void(const ExtractorSpec&)>& fn) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    for (const auto& kv : m_table) {
        if (kv.second->dataType == dataType) {
            fn(*kv.second);
        }
    }
}

}  // namespace dts::data
