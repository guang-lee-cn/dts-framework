#pragma once

#include <cstdint>
#include <functional>

namespace dts {

using PubCallback = std::function<void(const uint8_t* data, uint32_t len)>;

// ---- 线程状态 ----
enum class ThreadStatus : uint8_t { IDLE, WORKING, STOP };

// ---- 消息 ID：运行时线程内部只认 msgId，session 由静态路由绑定 ----
constexpr uint32_t MSG_ID_STATUS           = 0x0000;  // 状态更新
constexpr uint32_t MSG_ID_TASK_ACTIVE      = 0x0001;  // 调度 -> task
constexpr uint32_t MSG_ID_DATA_TASK_ACTIVE = 0x0002;  // task -> data
constexpr uint32_t MSG_ID_AGENT_DATA       = 0x0003;  // agent -> data
constexpr uint32_t MSG_ID_REPORT           = 0x0004;  // data -> 网管
constexpr uint32_t MSG_ID_LOG_COLLECT      = 0x0005;  // 调度 -> log
constexpr uint32_t MSG_ID_LOG_REPORT       = 0x0006;  // log -> 网管（log 线程 pub 能力）
constexpr uint32_t MSG_ID_TASK_CONFIG      = 0x0007;  // nfoam -> task 配置变更（32K JSON）
constexpr uint32_t MSG_ID_TASK_RESPONSE    = 0x0008;  // task -> nfoam 响应 JSON（双向收发）

// ---- 会话：统一寻址键 detmw::endpoint（detmw.h，== / hash / ToString 内置）----
constexpr const char* SESSION_TYPE_DTS  = "DTS";
constexpr const char* SESSION_INST_TASK = "task";
constexpr const char* SESSION_INST_DATA = "data";
constexpr const char* SESSION_INST_LOG  = "log";

// ---- 状态消息载荷 ----
struct StatusMsg {
    ThreadStatus status;
    uint8_t reserved[3];
};

// ---- 数据大类（DataType）：大结构体头携带，工厂按此匹配 dataId ----
constexpr uint16_t TYPE_CELL  = 0x10;   // 小区级
constexpr uint16_t TYPE_UE    = 0x20;   // UE 级
constexpr uint16_t TYPE_OTHER = 0x30;   // 其它

// ---- 业务 dataId ----
constexpr uint16_t DATA_ID_CELL_PRB = 0x0011;  // 小区管道
constexpr uint16_t DATA_ID_UE_BLER  = 0x0021;  // UE 管道
constexpr uint16_t DATA_ID_UE_OTHER = 0x0031;  // 未注册、未跟踪 -> 演示过滤丢弃

// ---- 业务数据 ----
struct CellPrbData {
    uint16_t cellId;
    uint32_t prb;
};

struct UeBlerData {
    uint16_t ueId;
    uint16_t bler;
};

// 大结构体（<=32k）：SPA -> bb-agent -> data 线程，头带 dataType + 索引表，帧流可拼接
constexpr uint32_t BIG_FRAME_MAGIC = 0xD7D7D7D7;

struct BigFrameHeader {
    uint32_t magic;
    uint16_t dataType;   // 数据大类，工厂按此匹配
    uint16_t pad;
    uint32_t totalLen;
    uint32_t dataCount;
    struct Entry {
        uint16_t dataId;
        uint16_t pad;
        uint32_t offset;
        uint32_t len;
    };
    Entry entries[32];
};

// task 激活消息（调度平台 -> task 线程 -> data 线程）
struct TaskActiveMsg {
    uint16_t taskId;
    uint16_t type;
    uint16_t dataIdCount;
    uint16_t dataIds[8];
};

}  // namespace dts
