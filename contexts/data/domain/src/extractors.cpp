#include "extractor.h"
#include "extractor_registry.h"

#include <cstring>
#include <memory>

#include "data_ids.h"
#include "log.h"

namespace dts::data {

// cell 切片：raw 布局 [head 10B][Σ cacheSize_i 切片]（32K）。
// extractor i 切 raw 的 CellRawOffset(i) 偏移，长度 cacheSize[i]（4-200B 变长）。
class CCellSlice : public Extractor {
public:
    int Extra(const void* rawData, uint32_t len, void* cache) override {
        if (rawData == nullptr || len < m_offset + m_size) {
            return -1;
        }
        std::memcpy(cache, static_cast<const uint8_t*>(rawData) + m_offset, m_size);
        return static_cast<int>(m_size);
    }
    void SetSlice(uint32_t offset, uint32_t size) {
        m_offset = offset;
        m_size = size;
    }

private:
    uint32_t m_offset = 0;
    uint32_t m_size = 0;
};

// 显式调用：强制 extractors.o 链接 + 运行时注册全部 cell extractor（变长切片）。
void InitExtractors() {
    auto& reg = ExtractorRegistry::Instance();
    int n = 0;
    for (uint16_t i = 0; i < kCellDataIdCount; ++i) {
        const uint16_t id = kCellDataIdBase + i;
        const DataIdSpec s = SpecOf(id);
        if (s.dataId == 0) {
            continue;
        }
        auto proc = std::make_unique<CCellSlice>();
        proc->SetSlice(CellRawOffset(id), s.cacheSize);  // raw 内偏移 + 切片长度
        reg.Register(id, s.dataType, s.cacheSize, s.periodTicks, s.needCache, std::move(proc));
        ++n;
    }
    dts::log::Info("[data] InitExtractors registered={} cell dataId", n);
}

}  // namespace dts::data
