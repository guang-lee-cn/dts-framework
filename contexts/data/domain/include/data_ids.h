#pragma once

#include <cstdint>

#include "data_config.h"
#include "data_model.h"

// dataId 清单：列表驱动（模拟 100 个：cell 50 / ue 50，周期 1S/5S，needcache 混合）。
// 改列表即可增删 dataId；换真实清单只改此处。
// X(dataId, 类型名, 周期tick数, needcache)

#define DTS_CELL_DATA_IDS(X)                                    \
    X(1, CellData01, 10, true) X(2, CellData02, 50, false)      \
    X(3, CellData03, 10, true) X(4, CellData04, 50, false)      \
    X(5, CellData05, 10, true) X(6, CellData06, 50, false)      \
    X(7, CellData07, 10, true) X(8, CellData08, 50, false)      \
    X(9, CellData09, 10, true) X(10, CellData10, 50, false)     \
    X(11, CellData11, 10, true) X(12, CellData12, 50, false)    \
    X(13, CellData13, 10, true) X(14, CellData14, 50, false)    \
    X(15, CellData15, 10, true) X(16, CellData16, 50, false)    \
    X(17, CellData17, 10, true) X(18, CellData18, 50, false)    \
    X(19, CellData19, 10, true) X(20, CellData20, 50, false)    \
    X(21, CellData21, 10, true) X(22, CellData22, 50, false)    \
    X(23, CellData23, 10, true) X(24, CellData24, 50, false)    \
    X(25, CellData25, 10, true) X(26, CellData26, 50, false)    \
    X(27, CellData27, 10, true) X(28, CellData28, 50, false)    \
    X(29, CellData29, 10, true) X(30, CellData30, 50, false)    \
    X(31, CellData31, 10, true) X(32, CellData32, 50, false)    \
    X(33, CellData33, 10, true) X(34, CellData34, 50, false)    \
    X(35, CellData35, 10, true) X(36, CellData36, 50, false)    \
    X(37, CellData37, 10, true) X(38, CellData38, 50, false)    \
    X(39, CellData39, 10, true) X(40, CellData40, 50, false)    \
    X(41, CellData41, 10, true) X(42, CellData42, 50, false)    \
    X(43, CellData43, 10, true) X(44, CellData44, 50, false)    \
    X(45, CellData45, 10, true) X(46, CellData46, 50, false)    \
    X(47, CellData47, 10, true) X(48, CellData48, 50, false)    \
    X(49, CellData49, 10, true) X(50, CellData50, 50, false)

#define DTS_UE_DATA_IDS(X)                                          \
    X(1000, UeData01, 10, true) X(1001, UeData02, 50, false)        \
    X(1002, UeData03, 10, true) X(1003, UeData04, 50, false)        \
    X(1004, UeData05, 10, true) X(1005, UeData06, 50, false)        \
    X(1006, UeData07, 10, true) X(1007, UeData08, 50, false)        \
    X(1008, UeData09, 10, true) X(1009, UeData10, 50, false)        \
    X(1010, UeData11, 10, true) X(1011, UeData12, 50, false)        \
    X(1012, UeData13, 10, true) X(1013, UeData14, 50, false)        \
    X(1014, UeData15, 10, true) X(1015, UeData16, 50, false)        \
    X(1016, UeData17, 10, true) X(1017, UeData18, 50, false)        \
    X(1018, UeData19, 10, true) X(1019, UeData20, 50, false)        \
    X(1020, UeData21, 10, true) X(1021, UeData22, 50, false)        \
    X(1022, UeData23, 10, true) X(1023, UeData24, 50, false)        \
    X(1024, UeData25, 10, true) X(1025, UeData26, 50, false)        \
    X(1026, UeData27, 10, true) X(1027, UeData28, 50, false)        \
    X(1028, UeData29, 10, true) X(1029, UeData30, 50, false)        \
    X(1030, UeData31, 10, true) X(1031, UeData32, 50, false)        \
    X(1032, UeData33, 10, true) X(1033, UeData34, 50, false)        \
    X(1034, UeData35, 10, true) X(1035, UeData36, 50, false)        \
    X(1036, UeData37, 10, true) X(1037, UeData38, 50, false)        \
    X(1038, UeData39, 10, true) X(1039, UeData40, 50, false)        \
    X(1040, UeData41, 10, true) X(1041, UeData42, 50, false)        \
    X(1042, UeData43, 10, true) X(1043, UeData44, 50, false)        \
    X(1044, UeData45, 10, true) X(1045, UeData46, 50, false)        \
    X(1046, UeData47, 10, true) X(1047, UeData48, 50, false)        \
    X(1048, UeData49, 10, true) X(1049, UeData50, 50, false)

namespace dts::data {

// 生成 dataId 结构（模拟阶段同构：2 个 int；真实清单替换这里）
#define DEFINE_CACHE(id, Name, period, need) \
    struct Name { int f0; int f1; };
DTS_CELL_DATA_IDS(DEFINE_CACHE)
DTS_UE_DATA_IDS(DEFINE_CACHE)
#undef DEFINE_CACHE

// 注册规格表：dataId → (dataType, cacheSize, 周期, needcache)，供工厂/内存层用
struct DataIdSpec {
    uint16_t dataId;
    uint16_t dataType;      // DataType::CELL / UE
    uint32_t cacheSize;     // sizeof(dataId 结构)
    uint32_t periodTicks;   // 上报周期（tick 数）
    bool needCache;         // 是否 hash 槽位缓存
};

#define SPEC_CELL(id, Name, period, need) { id, static_cast<uint16_t>(DataType::CELL), sizeof(Name), period, need },
inline constexpr DataIdSpec kCellSpecs[] = { DTS_CELL_DATA_IDS(SPEC_CELL) };
#undef SPEC_CELL

#define SPEC_UE(id, Name, period, need) { id, static_cast<uint16_t>(DataType::UE), sizeof(Name), period, need },
inline constexpr DataIdSpec kUeSpecs[] = { DTS_UE_DATA_IDS(SPEC_UE) };
#undef SPEC_UE

}  // namespace dts::data
