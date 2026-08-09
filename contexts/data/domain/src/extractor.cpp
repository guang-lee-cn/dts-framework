#include "extractor.h"

#include <cstring>

#include "data_model.h"

namespace dts::data {

// 默认 Report：cache → 上报缓存 dst（前置子头，再拷 cache）
// 子头布局：SubHeader{dataId, len} + data；dst 由工厂定位到当前写入位
int Extractor::Report(const void* cache, uint32_t len, void* dst, uint32_t cap) {
    if (dst == nullptr || cap < sizeof(SubHeader) + len) {
        return -1;  // 容量不足（超 ≤32k，工厂侧拆分/丢弃策略）
    }
    auto* p = static_cast<uint8_t*>(dst);
    SubHeader sh{};
    sh.len = len;  // dataId 由工厂填（基类不知当前 dataId）
    std::memcpy(p, &sh, sizeof(sh));
    std::memcpy(p + sizeof(SubHeader), cache, len);
    return static_cast<int>(sizeof(SubHeader) + len);
}

}  // namespace dts::data
