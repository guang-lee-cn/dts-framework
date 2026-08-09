#pragma once

#include <cstdint>

namespace dts::data {

// 加工基类：rawData 切片 → cache 槽。data 线程独占调用，无锁。
// 默认实现三件（Extra/Hton/Report）；用户重写则覆盖，未重写出错责任在用户。
class Extractor {
public:
    virtual ~Extractor() = default;

    // 阶段一：从 rawData 提取本 dataId 切片写入 cache 槽。返回写入字节数（<0 失败）
    virtual int Extra(const void* rawData, void* cache) { (void)rawData; (void)cache; return 0; }
    // 字节序转换（cache 原地）。返回处理后字节数；默认本机序无操作
    virtual int Hton(void* cache, uint32_t len) { (void)cache; return static_cast<int>(len); }
    // 阶段二：cache → 上报缓存（ReportBuf::payload 追加 子头+data）。返回追加字节数（<0 失败）
    // dst 指向上报缓存当前写入位，cap 为剩余容量；写入后由工厂推进 used
    virtual int Report(const void* cache, uint32_t len, void* dst, uint32_t cap);
};

}  // namespace dts::data
