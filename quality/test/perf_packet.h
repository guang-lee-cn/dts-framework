#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "dts_def.h"

namespace dts {

// perf 链路单包：32k。前 sizeof(TaskActiveMsg) 字节为合法 TaskActiveMsg 头
// （task/data 线程现有 handler 能解析，dataIdCount<=8），其后 seq + t_send 打点。
constexpr size_t kPerfPacketSize = 32 * 1024;
constexpr uint32_t kPerfDoneSeq = 0xFFFFFFFF;  // DONE 标记包

// 字段偏移（前部为 TaskActiveMsg，其余按 int 字节数拆分）
constexpr size_t kPerfSeqOff = sizeof(TaskActiveMsg);  // uint32
constexpr size_t kPerfTsOff = kPerfSeqOff + 4;         // uint64

inline void PerfSetSeq(uint8_t* pkt, uint32_t seq) {
    std::memcpy(pkt + kPerfSeqOff, &seq, sizeof(seq));
}
inline uint32_t PerfGetSeq(const uint8_t* pkt) {
    uint32_t v;
    std::memcpy(&v, pkt + kPerfSeqOff, sizeof(v));
    return v;
}
inline void PerfSetTs(uint8_t* pkt, uint64_t tsUs) {
    std::memcpy(pkt + kPerfTsOff, &tsUs, sizeof(tsUs));
}
inline uint64_t PerfGetTs(const uint8_t* pkt) {
    uint64_t v;
    std::memcpy(&v, pkt + kPerfTsOff, sizeof(v));
    return v;
}

// 填充包：TaskActiveMsg 头（taskId 由调用方每次改写，保证唯一不拒） + seq + 打点
// size 必须是调用方缓冲区实际大小（曾用 kPerfPacketSize 固定 memset 溢出小包，务注意）
inline void PerfInit(uint8_t* pkt, size_t size) {
    auto* ta = reinterpret_cast<TaskActiveMsg*>(pkt);
    std::memset(pkt, 0, size);
    ta->type = 0;
    ta->dataIdCount = 1;
    ta->dataIds[0] = DATA_ID_CELL_PRB;
}

// ---- task 配置变更基准：32K JSON 载荷，task 简析 JSON + 响应 ----
// 格式：{"taskId":<id>,"seq":<s>,"t_send":<us>,"pad":"<填充到 size-3>"}
inline void PerfBuildConfigJson(uint8_t* out, size_t size, uint32_t taskId, uint32_t seq,
                                uint64_t tSendUs) {
    int n = std::snprintf(reinterpret_cast<char*>(out), size,
                          "{\"taskId\":%u,\"seq\":%u,\"t_send\":%llu,\"pad\":\"", taskId, seq,
                          static_cast<unsigned long long>(tSendUs));
    if (n < 0 || static_cast<size_t>(n) >= size - 3) return;
    for (size_t i = static_cast<size_t>(n); i < size - 3; ++i) {
        out[i] = 'a';
    }
    out[size - 3] = '"';
    out[size - 2] = '}';
    out[size - 1] = '\0';
}

// 从 JSON 里取整数字段值（简单扫描，如 "seq":123）
inline uint64_t PerfJsonInt(const uint8_t* p, const char* key, uint64_t fallback) {
    const char* s = reinterpret_cast<const char*>(p);
    const char* hit = std::strstr(s, key);
    if (hit == nullptr) return fallback;
    const char* colon = std::strchr(hit, ':');
    if (colon == nullptr) return fallback;
    return std::strtoull(colon + 1, nullptr, 10);
}

}  // namespace dts
