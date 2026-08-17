#pragma once

#include <cstddef>
#include <cstdint>

// data 子系统集中配置：默认实现 + 预留配置接口（改一两行即换自定义，运行时 console set 可调）
namespace dts::data {

// 域容量（各域独立核算，不共享）
inline constexpr uint32_t kCellSlotCap = 384;   // 逻辑小区上限
inline constexpr uint32_t kUeSlotCap = 2000;    // UE 上限

// 周期（tick 数，tick 粒度 100ms）
inline constexpr uint32_t kTickMs = 100;
inline constexpr uint32_t kPeriod1S = 10;
inline constexpr uint32_t kPeriod5S = 50;

// 超周期 TTL：加工缓存槽连续 N 个上报周期未更新即失效（防死数据占槽）
inline constexpr uint32_t kTtlPeriods = 3;

// 帧处理预算（需求口径：data 线程内单帧处理，出 mailbox → 处理完成，含业务代码）：
//   kFrameBudgetWarnUs  预警阈值（接近上限，滚雪球信号）
//   kFrameBudgetAlarmUs 告警阈值（超预算 = 确定性违约，计数上报 OAM）
inline constexpr uint64_t kFrameBudgetWarnUs = 3000;
inline constexpr uint64_t kFrameBudgetAlarmUs = 5000;

// 加工缓存池上限（可配；容量核算 15~80MB < 100MB）
inline constexpr size_t kMemCapBytes = 100 * 1024 * 1024;

// 测量对象 hash 键（默认实现：cellId 高位 + cpId/ueId 低位；换自定义改这里）
inline uint64_t CellHashKey(uint32_t cellId, uint32_t cpId) {
    return (static_cast<uint64_t>(cellId) << 32) | cpId;
}
inline uint64_t UeHashKey(uint32_t cellId, uint32_t ueId) {
    return (static_cast<uint64_t>(cellId) << 32) | ueId;
}

}  // namespace dts::data
