#pragma once

#include <cstdint>

#include "data_model.h"

namespace dts::data {

// data 线程内存管理专门类：加工缓存池（测量对象当前值）。
// 全部内存启动时预分配（DataMemManager::Init），data 线程运行期零分配。
// 注：周期汇聚上报缓存（ReportBuf）搁置，当前直通模式 Report 经 Extractor 基类直推 webserver。
class DataMemManager {
public:
    static DataMemManager& Instance();

    // 按 dataId spec 表构建 (域×周期) hash region + needcache=false 固定槽，分配 baseAddr（一次性）
    void Init();

    // ---- 加工缓存：needcache=true，hash 槽位（线性探测，零分配）----
    // 定位/创建测量对象块；命中刷新 TTL，新建置 lastTick。满则返回 nullptr
    CacheHead* AcquireBlock(uint16_t domain, uint32_t periodTicks, uint64_t hashKey, uint32_t nowTick);
    // 仅查找（不创建/不刷新）
    CacheHead* FindBlock(uint16_t domain, uint32_t periodTicks, uint64_t hashKey) const;
    // 块内 dataId 槽地址（block 须由 Acquire/Find 返回）
    void* SlotOf(const CacheHead* block, uint16_t dataId);

    // ---- 加工缓存：needcache=false，每 dataId 一个固定槽 ----
    void* FixedSlot(uint16_t dataId);

    // TTL：标记超期槽失效（定时器 nowTick 驱动，N 周期未更新归还）
    void EvictExpired(uint32_t nowTick);

private:
    DataMemManager() = default;
    struct Impl;
    Impl* m_impl = nullptr;
};

}  // namespace dts::data
