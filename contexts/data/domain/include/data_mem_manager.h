#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include "data_config.h"
#include "data_model.h"

namespace dts::data {

// 上报缓存：per-task ≤32k，定时器 1s 推远端清空（payload 累积 n×(子头+dataId)）
struct ReportBuf {
    static constexpr size_t kCap = 32 * 1024;
    ReportHeader header{};
    uint8_t payload[kCap]{};
    uint32_t used = 0;  // payload 有效字节
};

// data 线程内存管理专门类：加工缓存池（测量对象当前值）+ 上报缓存池（待推送快照）。
// 加工缓存 = 源，上报缓存 = 快照，Report 做"源→快照"拷贝；上报集合 = 加工缓存 ∩ 激活集。
// 全部内存启动时预分配（DataMemManager::Init），data 线程运行期零分配（ReportBuf 在 control path 分配）。
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

    // ---- 上报缓存：per-task（任务创建时 Acquire，control path 分配）----
    ReportBuf* AcquireReport(uint32_t taskId);
    void ReleaseReport(uint32_t taskId);
    void ForEachReport(const std::function<void(ReportBuf&)>& fn);

private:
    DataMemManager() = default;
    struct Impl;
    Impl* m_impl = nullptr;
};

}  // namespace dts::data
