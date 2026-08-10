#include "extractor.h"
#include "extractor_registry.h"

#include <cstring>
#include <memory>

#include "data_ids.h"
#include "log.h"

namespace dts::data {

// cell 切片：raw 布局 [u16 dataType][u32 cellId][u32 cpId][N × 8字节切片]，
// 每个 extractor 切 raw 的固定偏移（m_offset），写入 cache 8 字节。
// raw 头部大小（跳过 dataType+cellId+cpId）；偏移由注册时按 dataId 序号设置
class CCellSlice : public Extractor {
public:
    int Extra(const void* rawData, uint32_t len, void* cache) override {
        if (rawData == nullptr || len < m_offset + 8) {
            return -1;
        }
        std::memcpy(cache, static_cast<const uint8_t*>(rawData) + m_offset, 8);
        return 8;
    }
    void SetOffset(uint32_t off) { m_offset = off; }

private:
    uint32_t m_offset = 0;
};

// 显式调用：强制本 TU 链接 + 运行时注册全部 cell extractor（按 dataId 序号设切片偏移）。
// 必须调（run.cpp 装配时），否则 ExtractorRegistry 空。
void InitExtractors() {
    constexpr uint32_t kHead = sizeof(uint16_t) + sizeof(uint32_t) * 2;  // dataType + cellId + cpId
    auto& reg = ExtractorRegistry::Instance();
    int n = 0;
    for (uint16_t i = 0; i < kCellDataIdCount; ++i) {
        const uint16_t id = kCellDataIdBase + i;
        const DataIdSpec* s = SpecOf(id);
        if (s == nullptr) {
            continue;
        }
        auto proc = std::make_unique<CCellSlice>();
        proc->SetOffset(kHead + i * 8);  // dataId i 切 raw 第 i 个 8 字节切片
        reg.Register(id, s->dataType, s->cacheSize, s->periodTicks, s->needCache, std::move(proc));
        ++n;
    }
    dts::log::Info("[data] InitExtractors registered={} cell dataId", n);
}

}  // namespace dts::data
