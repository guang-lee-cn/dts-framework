#pragma once

#include <cstdint>

#include "data_config.h"
#include "data_model.h"

// dataId 清单：cell 500 个（id 1-500），cacheSize 随机 4-200B（固定 seed 42，Σ=32758=raw32K-head）。
// raw 布局：[u16 dataType][u32 cellId][u32 cpId][Σ cacheSize_i 切片] = 32768 字节（32K）。
// extractor i 切 raw 偏移 head+Σ(prev cacheSize)，长度 cacheSize[i]（data/spa 共享此 layout）。

namespace dts::data {

inline constexpr uint16_t kCellDataIdBase = 1;
inline constexpr uint16_t kCellDataIdCount = 500;
inline constexpr uint32_t kRawLen = 32768;  // raw 帧固定 32K

// 每个 cell dataId 的 cache 大小（4-200B 随机，Σ = 32K - head 10 = 32758）
inline constexpr uint16_t kCellCacheSizes[] = {
    167,32,10,193,74,66,61,39,192,30,177,193,143,26,155,112,12,11,27,59,
    63,133,158,10,147,54,187,170,183,143,111,60,118,154,75,5,198,44,182,112,
    91,75,43,59,199,90,30,27,101,28,95,92,158,71,15,190,121,141,35,100,
    24,145,79,164,162,96,151,53,184,21,15,173,62,78,24,63,29,101,75,120,
    166,97,45,98,94,57,175,72,183,178,169,22,159,166,47,140,190,66,45,122,
    101,73,167,180,146,60,179,87,200,18,62,12,84,106,72,20,58,149,187,84,
    58,171,131,105,168,121,40,71,39,67,194,147,141,71,195,153,113,153,106,96,
    60,39,134,130,27,197,16,32,43,164,44,178,112,156,20,102,101,156,123,139,
    68,145,6,178,188,33,178,141,196,72,200,168,91,32,79,115,44,120,4,188,
    188,71,132,199,49,133,31,164,80,167,133,159,54,43,99,199,45,142,139,4,
    157,86,129,8,32,96,82,65,18,65,149,24,25,191,128,21,198,140,200,36,
    36,172,125,144,46,71,139,159,112,58,142,197,190,180,55,186,83,106,175,170,
    99,116,136,119,34,67,61,20,90,9,154,145,62,154,60,5,22,185,165,19,
    62,21,12,88,22,135,64,75,175,128,58,142,37,189,150,151,125,66,125,108,
    52,28,28,172,114,94,112,109,123,190,17,176,171,169,29,19,107,190,90,31,
    67,53,52,141,118,39,112,50,75,122,67,23,117,107,5,4,9,4,4,4,
    4,5,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
    4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
    4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
    4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
    4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
    4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
    4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
    4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
    4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4
};

// 注册规格
struct DataIdSpec {
    uint16_t dataId;
    uint16_t dataType;      // DataType::CELL
    uint32_t cacheSize;     // 切片大小（4-200B）
    uint32_t periodTicks;   // 1S
    bool needCache;
};

// 构造 spec 表（运行时 / 编译期均可访问）
inline DataIdSpec SpecOf(uint16_t dataId) {
    const uint16_t i = dataId - kCellDataIdBase;
    if (i >= kCellDataIdCount) {
        return {0, 0, 0, 0, false};
    }
    return {dataId, static_cast<uint16_t>(DataType::CELL), kCellCacheSizes[i], 10, true};
}

// extractor i 在 raw 内的偏移（head + Σ prev cacheSize）
inline uint32_t CellRawOffset(uint16_t dataId) {
    const uint16_t i = dataId - kCellDataIdBase;
    uint32_t off = sizeof(uint16_t) + sizeof(uint32_t) * 2;  // head = 10
    for (uint16_t k = 0; k < i; ++k) {
        off += kCellCacheSizes[k];
    }
    return off;
}

}  // namespace dts::data
