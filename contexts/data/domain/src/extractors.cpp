#include "extractor.h"
#include "extractor_registry.h"

#include <cstring>
#include <memory>

#include "data_ids.h"
#include "data_model.h"

namespace dts::data {

// ---- 真实切片（演示）：dataId=1 CellData01 ----
// 帧: dataType(u16) + cellId(u32) + cpId(u32) + 数据(f0,f1)；Extra 取尾部两 int
class CCellData01 : public Extractor {
public:
    int Extra(const void* rawData, uint32_t len, void* cache) override {
        const size_t off = sizeof(uint16_t) + sizeof(uint32_t) * 2;  // 跳过 dataType + cellId + cpId
        if (rawData == nullptr || len < off + 2 * sizeof(int)) {
            return -1;
        }
        std::memcpy(cache, static_cast<const uint8_t*>(rawData) + off, 2 * sizeof(int));
        return 2 * static_cast<int>(sizeof(int));
    }
};
REGISTER_DATAID_EXTRACTOR(1, CCellData01, CellData01, CELL)

// ---- 真实切片（演示）：dataId=1000 UeData01 ----
class CUeData01 : public Extractor {
public:
    int Extra(const void* rawData, uint32_t len, void* cache) override {
        const size_t off = sizeof(uint16_t) + sizeof(uint32_t) * 2;  // 跳过 dataType + cellId + ueId
        if (rawData == nullptr || len < off + 2 * sizeof(int)) {
            return -1;
        }
        std::memcpy(cache, static_cast<const uint8_t*>(rawData) + off, 2 * sizeof(int));
        return 2 * static_cast<int>(sizeof(int));
    }
};
REGISTER_DATAID_EXTRACTOR(1000, CUeData01, UeData01, UE)

// ---- 其余 dataId 默认占位（空 Extra）：让全链路可跑 ----
// 类名加 Def 后缀避免与上方真实类冲突；dataId=1/1000 已被真实注册，默认注册静默失败
namespace {
#define DEFAULT_CELL_EXTRACTOR(id, Name, period, need)                                          \
    struct C##Name##Def : Extractor {};                                                         \
    struct C##Name##Def##_reg {                                                                 \
        C##Name##Def##_reg() {                                                                  \
            ExtractorRegistry::Instance().Register(                                             \
                id, static_cast<uint16_t>(DataType::CELL), sizeof(Name), period, need,          \
                std::make_unique<C##Name##Def>());                                              \
        }                                                                                       \
    };                                                                                          \
    C##Name##Def##_reg g_C##Name##Def##_reg;
DTS_CELL_DATA_IDS(DEFAULT_CELL_EXTRACTOR)
#undef DEFAULT_CELL_EXTRACTOR

#define DEFAULT_UE_EXTRACTOR(id, Name, period, need)                                            \
    struct C##Name##Def : Extractor {};                                                         \
    struct C##Name##Def##_reg {                                                                 \
        C##Name##Def##_reg() {                                                                  \
            ExtractorRegistry::Instance().Register(                                             \
                id, static_cast<uint16_t>(DataType::UE), sizeof(Name), period, need,            \
                std::make_unique<C##Name##Def>());                                              \
        }                                                                                       \
    };                                                                                          \
    C##Name##Def##_reg g_C##Name##Def##_reg;
DTS_UE_DATA_IDS(DEFAULT_UE_EXTRACTOR)
#undef DEFAULT_UE_EXTRACTOR
}  // namespace

// 显式调用：强制本 TU 链接（静态注册对象随 .o 在 main 前初始化）。
// 没有它，静态库 extractors.o 的匿名 _reg 符号无引用，链接器丢弃 → 注册表空。
void InitExtractors() {}

}  // namespace dts::data
