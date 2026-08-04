#include "scheduler_mock.h"

namespace dts {

void SchedulerMock::Start() {
    // TODO(platform): 模拟调度平台，经平台 pubsub 发送：
    //   1. 三线程 IDLE/WORKING 状态消息（{SESSION_TYPE_DTS, inst, MSG_ID_STATUS}, StatusMsg）
    //   2. task 激活（{SESSION_TYPE_DTS, SESSION_INST_TASK, MSG_ID_TASK_ACTIVE}, TaskActiveMsg）
    //   3. log 采集（{SESSION_TYPE_DTS, SESSION_INST_LOG, MSG_ID_LOG_COLLECT}, 1B）
    //   示例：pubsub.Publish({SESSION_TYPE_DTS, SESSION_INST_TASK, MSG_ID_TASK_ACTIVE}, &m, sizeof(m));
}

}  // namespace dts
