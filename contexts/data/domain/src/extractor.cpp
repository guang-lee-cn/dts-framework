#include "extractor.h"

#include <cstring>

#include "data_factory_v2.h"  // ReportSink（domain 内部头，无环）
#include "data_model.h"       // ReportHeader / SubHeader

namespace dts::data {

// 基类默认 Report：栈帧组 ReportHeader + SubHeader + cache → sink->Publish 直推 webserver。
// 所有 dataId 默认走此实现（子类不重写）；后续业务重构改本函数即可。
void Extractor::Report(const ReportCtx& ctx) {
    if (ctx.sink == nullptr || ctx.cache == nullptr) {
        return;
    }
    // 上限保护：单次上报 ≤32k（REPORT 通道约定），超限截断
    constexpr uint32_t kMaxPayload = 32 * 1024;
    const uint32_t dataLen = ctx.len > kMaxPayload ? kMaxPayload : ctx.len;
    const uint32_t total = sizeof(ReportHeader) + sizeof(SubHeader) + dataLen;

    uint8_t buf[32 * 1024 + sizeof(ReportHeader) + sizeof(SubHeader)];
    auto* hdr = reinterpret_cast<ReportHeader*>(buf);
    hdr->taskId = ctx.taskId;
    hdr->timestampMs = ctx.timestampMs;
    hdr->seq = ctx.seq;
    hdr->payloadLen = sizeof(SubHeader) + dataLen;

    auto* sh = reinterpret_cast<SubHeader*>(buf + sizeof(ReportHeader));
    sh->dataId = ctx.dataId;
    sh->len = dataLen;

    std::memcpy(buf + sizeof(ReportHeader) + sizeof(SubHeader), ctx.cache, dataLen);
    ctx.sink->Publish(buf, total);
}

}  // namespace dts::data
