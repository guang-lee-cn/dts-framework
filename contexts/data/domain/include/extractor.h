#pragma once

#include <cstdint>

namespace dts::data {

class ReportSink;  // forward-decl（domain 抽象，bootstrap 注入 DtsMw 适配）

// Report 调用上下文（工厂在调用点填，Extractor 全程无状态）
struct ReportCtx {
    const void* cache;     // Extra 写入的切片
    uint32_t len;          // cache 有效字节
    uint16_t dataId;       // 本切片 dataId
    uint32_t taskId;       // 当前任务（握手标记）
    uint64_t timestampMs;  // 时间戳
    uint32_t seq;          // 上报序号（工厂递增）
    ReportSink* sink;      // 上报出口（基类 Report 直推 webserver）
};

// 加工基类：rawData 切片 → cache 槽 → 上报。
// 默认实现三件（Extra/Hton/Report）；用户重写则覆盖，未重写出错责任在用户。
// Report 基类直推：cache → sink->Publish（总头+子头+data），所有 dataId 默认走基类，子类不重写。
class Extractor {
public:
    virtual ~Extractor() = default;

    // 阶段一：从 rawData（len 字节）提取本 dataId 切片写入 cache 槽。返回写入字节数（<0 失败）
    virtual int Extra(const void* rawData, uint32_t len, void* cache) {
        (void)rawData; (void)len; (void)cache; return 0;
    }
    // 字节序转换（cache 原地）。返回处理后字节数；默认本机序无操作
    virtual int Hton(void* cache, uint32_t len) { (void)cache; return static_cast<int>(len); }
    // 阶段二：cache → sink 直推（ReportHeader + SubHeader + data）。子类默认不重写
    virtual void Report(const ReportCtx& ctx);
};

}  // namespace dts::data
