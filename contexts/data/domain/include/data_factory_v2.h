#pragma once

#include <cstdint>
#include <vector>

namespace dts::data {

// 数据工厂：rawData 帧批处理。
// 流程：dataType 路由 → 激活集（该 dataType 下被跟踪的 dataId 并集）
//      → 命中加工类 → Extra 全加工入槽 → 周期到 → Report 分发 task 上报缓存
class DataFactory {
public:
    static DataFactory& Instance();

    // 任务消息（task 线程经 mailbox 投递）：data 线程内更新 TdMapV2
    void OnTask(uint32_t taskId, const std::vector<uint16_t>& dataIds, uint32_t periodMs);

    // 处理一帧 rawData（data 线程独占）。nowTick 用于周期判定/TTL
    void Process(const void* rawData, uint32_t len, uint16_t dataType, uint32_t nowTick);

    // 1s tick：推送各 task 上报缓存（阶段 5 接入定时器调用）
    void FlushReports();

private:
    DataFactory() = default;
    uint32_t m_tick = 0;
};

}  // namespace dts::data
