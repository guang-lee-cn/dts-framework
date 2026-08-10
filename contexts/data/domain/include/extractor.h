#pragma once

#include <cstdint>

namespace dts::data {

// 加工基类：rawData 切片 → cache 槽。data 线程独占调用，无锁。
// 只负责切分（Extra）+ 字节序（Hton）；上报聚合由 ReportAggregator 组件负责。
class Extractor {
public:
    virtual ~Extractor() = default;

    // 从 rawData（len 字节）提取本 dataId 切片写入 cache 槽。返回写入字节数（<0 失败）
    virtual int Extra(const void* rawData, uint32_t len, void* cache) {
        (void)rawData; (void)len; (void)cache; return 0;
    }
    // 字节序转换（cache 原地）。返回处理后字节数；默认本机序无操作
    virtual int Hton(void* cache, uint32_t len) { (void)cache; return static_cast<int>(len); }
};

}  // namespace dts::data
