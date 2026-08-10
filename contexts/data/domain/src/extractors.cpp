#include "extractor.h"
#include "extractor_registry.h"

#include <cstring>
#include <memory>

#include "data_ids.h"

namespace dts::data {

// 通用 cell 切片：raw 布局 [u16 dataType][u32 cellId][u32 cpId][数据...]，Extra 取头部之后 8 字节。
// 极限测试：10 个 dataId 共用同一切片逻辑（report 带不同 dataId），量通道消息率极限。
class CCellSlice : public Extractor {
public:
    int Extra(const void* rawData, uint32_t len, void* cache) override {
        const size_t off = sizeof(uint16_t) + sizeof(uint32_t) * 2;  // 跳过 dataType + cellId + cpId
        if (rawData == nullptr || len < off + 8) {
            return -1;
        }
        std::memcpy(cache, static_cast<const uint8_t*>(rawData) + off, 8);
        return 8;
    }
};

// 固定 10 个 cell dataId（id 1-10），各注册一个 CCellSlice（dataId 不同，切同一偏移）
#define CELL_SLICE_10(X)                                                  \
    X(1, CellData01) X(2, CellData02) X(3, CellData03) X(4, CellData04)   \
    X(5, CellData05) X(6, CellData06) X(7, CellData07) X(8, CellData08)   \
    X(9, CellData09) X(10, CellData10)

#define REG_CELL_SLICE(id, Name)                                                  \
    namespace {                                                                   \
    struct Name##_reg_t {                                                         \
        Name##_reg_t() {                                                          \
            const auto* s = SpecOf(id);                                           \
            ExtractorRegistry::Instance().Register(                               \
                id, static_cast<uint16_t>(DataType::CELL), sizeof(Name),          \
                s->periodTicks, s->needCache, std::make_unique<CCellSlice>());    \
        }                                                                         \
    } g_##Name##_reg;                                                             \
    }
CELL_SLICE_10(REG_CELL_SLICE)
#undef REG_CELL_SLICE
#undef CELL_SLICE_10

// 显式调用：强制本 TU 链接（静态注册对象随 .o 在 main 前初始化）。
// 没有它，静态库 extractors.o 的匿名 _reg 符号无引用，链接器丢弃 → 注册表空。
void InitExtractors() {}

}  // namespace dts::data
