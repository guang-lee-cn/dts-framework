#include "scheduler_mock.h"

#include <chrono>
#include <cstring>
#include <thread>

#include "detmw.h"
#include "dts_def.h"
#include "dts_mw.h"

namespace dts {

void SchedulerMock::Start() {
    // 模拟调度平台：task 激活（msg1）→ task 线程建任务 → publish_internal 直通 data（握手）。
    // 等组合根装配完成（DtsMw 在 Run 装配早期设置）；发现完成前消息会丢，故周期重发
    // （TaskFactory 对重复 taskId 幂等拒绝，无害）。
    for (int i = 0; i < 100 && DtsMw() == nullptr; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (DtsMw() == nullptr) {
        return;
    }
    TaskActiveMsg m{};
    m.taskId = 1;
    m.type = 0;
    m.dataIdCount = 1;
    m.dataIds[0] = DATA_ID_CELL_PRB;  // legacy 常量，仅激活演示用
    for (int i = 0; i < 6; ++i) {
        DtsMw()->publish_external(detmw::endpoint{SESSION_TYPE_DTS, SESSION_INST_TASK,
                                                  MSG_ID_TASK_ACTIVE},
                                  reinterpret_cast<const uint8_t*>(&m), sizeof(m));
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

}  // namespace dts
