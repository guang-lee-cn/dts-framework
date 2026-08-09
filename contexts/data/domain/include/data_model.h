#pragma once

#include <cstdint>

#include "data_config.h"

namespace dts::data {

// 数据大类（rawData 第一层路由）
enum class DataType : uint16_t { CELL = 0, UE = 1 /* 未来扩展 */ };

// 测量对象头（CacheHead 的 union 成员，hash 键来源）
struct CellHead {
    uint32_t cellId;
    uint32_t cpId;
};
struct UeHead {
    uint32_t cellId;
    uint32_t ueId;
};

// 加工缓存块头：一个测量对象一块，头下挂该对象的所有 dataId 切片
struct CacheHead {
    DataType type;
    union {
        CellHead cell;
        UeHead ue;
    };
    uint32_t lastTick = 0;    // TTL：最后更新 tick（nowTick）
    uint32_t periodTicks = 0; // 本块上报周期（tick 数，1S=10 / 5S=50）
};

// 上报子头：总头 + n × (子头 + dataId 数据)
struct SubHeader {
    uint16_t dataId;
    uint16_t len;
};

// 上报总头
struct ReportHeader {
    uint32_t taskId;
    uint64_t timestampMs;
    uint32_t seq;
    uint32_t payloadLen;
};

}  // namespace dts::data
