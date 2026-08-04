#include <spdlog/spdlog.h>

#include "dts_mw.h"
#include "task_msg_handler.h"

#include <cstdio>
#include <cstring>

#include "task_factory.h"
#include "task_manager.h"
#include "task_strategy.h"

namespace dts {

namespace {

// 状态消息由线程体更新线程状态，业务层不处理（MSG_ID_STATUS）

void OnTaskActive(const uint8_t* msg, uint32_t len) {
    if (msg == nullptr || len < sizeof(TaskActiveMsg)) return;
    TaskActiveMsg m;
    std::memcpy(&m, msg, sizeof(m));
    if (m.dataIdCount > 8) return;

    TaskConfig cfg;
    cfg.taskId = m.taskId;
    cfg.type = m.type;
    cfg.dataIds.assign(m.dataIds, m.dataIds + m.dataIdCount);

    if (!TaskFactory().Create(TaskManager::Instance(), cfg)) {
        spdlog::info("[task:handler] task {} rejected (duplicate or full)", cfg.taskId);
        return;
    }
    DefaultTaskStrategy().OnTaskCreated(cfg);

    // 经 detmw 投递给 data 线程（对齐 itran 回调模型）
    if (DtsMw() != nullptr) {
        detmw_publish(DtsMw(), SESSION_TYPE_DTS, SESSION_INST_DATA, MSG_ID_DATA_TASK_ACTIVE, msg, len);
    }
}

// 配置变更（nfoam → task，32K JSON）：简析 JSON 模拟业务 parse 耗时，响应 JSON 给 nfoam（双向收发）
void OnTaskConfig(const uint8_t* msg, uint32_t len) {
    if (msg == nullptr || len == 0) return;
    // 简析：扫描 "taskId" 字段位置，模拟真实 JSON parse 的 CPU 成本（真实业务比这重）
    const char* s = reinterpret_cast<const char*>(msg);
    uint32_t keyPos = 0;
    for (uint32_t i = 0; i + 8 < len; ++i) {
        if (s[i] == '"' && std::memcmp(s + i + 1, "taskId", 6) == 0) {
            keyPos = i;
            break;
        }
    }
    spdlog::info("[task:handler] config parsed (taskId@{} / {}B)", keyPos, len);
    // 响应：回传 JSON（反向 32K 通路同样压）
    if (DtsMw() != nullptr) {
        detmw_publish(DtsMw(), SESSION_TYPE_DTS, SESSION_INST_TASK, MSG_ID_TASK_RESPONSE, msg, len);
    }
}

}  // namespace

void TaskMsgHandlerDispatch(ThreadStatus status, uint32_t msgId, const uint8_t* msg, uint32_t len) {
    (void)status;
    // 平台 pubsub 已路由到本线程，按 msgId 直分（MSG_ID_STATUS 由线程体更新状态）
    if (msgId == MSG_ID_TASK_ACTIVE) {
        OnTaskActive(msg, len);
    } else if (msgId == MSG_ID_TASK_CONFIG) {
        OnTaskConfig(msg, len);
    }
}

}  // namespace dts
