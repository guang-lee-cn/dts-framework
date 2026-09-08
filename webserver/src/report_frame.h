// DTS_data_4 上报帧解析（只读视图，不拷贝）。
// 帧格式（与 data 线程 ReportAggregator 对齐，结构定义复用 data_model.h）：
//   ReportHeader(20B: taskId u32 / timestampMs u64 / seq u32 / payloadLen u32)
//   + n × (SubHeader(4B: dataId u16 / len u16) + 切片数据)
// 注意：总头与子头为原生字节序；切片体经 Hton 网络序（v1 观测面不解码体内容）。
#pragma once

#include <cstdint>
#include <cstring>

#include "data_model.h"

namespace web {

struct ReportView {
    uint32_t taskId = 0;
    uint64_t timestampMs = 0;
    uint32_t seq = 0;
    uint32_t payloadLen = 0;
    uint16_t sliceCount = 0;   // 有效切片数
    uint16_t dataIdMin = 0;    // 切片 dataId 范围（观测面速览）
    uint16_t dataIdMax = 0;
    uint32_t frameLen = 0;     // 整帧字节数
};

// 解析成功返回 true；长度不足/内部越界返回 false（out 保持零值）
inline bool ParseReport(const uint8_t* data, uint32_t len, ReportView& out) {
    if (data == nullptr || len < sizeof(dts::data::ReportHeader)) {
        return false;
    }
    dts::data::ReportHeader hdr;
    std::memcpy(&hdr, data, sizeof(hdr));
    out.taskId = hdr.taskId;
    out.timestampMs = hdr.timestampMs;
    out.seq = hdr.seq;
    out.payloadLen = hdr.payloadLen;
    out.frameLen = len;

    const uint32_t body = len - sizeof(hdr);
    uint32_t pos = 0;
    uint16_t minId = 0xFFFF;
    uint16_t maxId = 0;
    while (pos + sizeof(dts::data::SubHeader) <= body && out.sliceCount < 1000) {
        dts::data::SubHeader sub;
        std::memcpy(&sub, data + sizeof(hdr) + pos, sizeof(sub));
        if (pos + sizeof(sub) + sub.len > body) {
            break;  // 尾部截断帧：按已解析计数
        }
        if (sub.dataId < minId) {
            minId = sub.dataId;
        }
        if (sub.dataId > maxId) {
            maxId = sub.dataId;
        }
        out.sliceCount++;
        pos += sizeof(sub) + sub.len;
    }
    out.dataIdMin = out.sliceCount > 0 ? minId : 0;
    out.dataIdMax = maxId;
    return true;
}

}  // namespace web
