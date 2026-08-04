#pragma once

namespace dts {

// 模拟外部调度平台：上电发 IDLE/WORKING 状态 + task 激活 + log 采集
class SchedulerMock {
public:
    void Start();
};

}  // namespace dts
