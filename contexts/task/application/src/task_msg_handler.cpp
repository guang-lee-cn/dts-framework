#include "task_msg_handler.h"

#include <cstring>

#include "dts_mw.h"
#include "log.h"
#include "task_factory.h"
#include "task_manager.h"
#include "task_strategy.h"

namespace dts {

namespace {

// ------------------------------------------------------------------
// 第三层路由处理函数：组内 msgId 命中后由消息表调用，签名 (void* data, uint32_t len)。
// 一个函数 = 一个消息流；状态消息（MSG_ID_STATUS）由线程体消费，不在此层。
// ------------------------------------------------------------------

void OnTaskActive(void* data, uint32_t len) {
    if (data == nullptr || len < sizeof(TaskActiveMsg)) {
        return;
    }
    TaskActiveMsg m;
    std::memcpy(&m, data, sizeof(m));
    if (m.dataIdCount > 8) {
        return;
    }

    TaskConfig cfg;
    cfg.taskId = m.taskId;
    cfg.type = m.type;
    cfg.dataIds.assign(m.dataIds, m.dataIds + m.dataIdCount);

    if (!TaskFactory().Create(TaskManager::Instance(), cfg)) {
        dts::log::Info("[task:handler] task {} rejected (duplicate or full)", cfg.taskId);
        return;
    }
    DefaultTaskStrategy().OnTaskCreated(cfg);

    // 经 detmw 投递给 data 线程（进程内直通 mailbox，免序列化；对齐 itran 回调模型）
    if (DtsMw() != nullptr) {
        DtsMw()->publish_internal(detmw::endpoint{SESSION_TYPE_DTS, SESSION_INST_DATA,
                                                  MSG_ID_DATA_TASK_ACTIVE},
                                  static_cast<const uint8_t*>(data), len);
    }
}

// 配置变更（nfoam → task，32K JSON）：简析 JSON 模拟业务 parse 耗时，响应 JSON 给 nfoam（双向收发）
void OnTaskConfig(void* data, uint32_t len) {
    if (data == nullptr || len == 0) {
        return;
    }
    // 简析：扫描 "taskId" 字段位置，模拟真实 JSON parse 的 CPU 成本（真实业务比这重）
    const char* s = static_cast<const char*>(data);
    uint32_t keyPos = 0;
    for (uint32_t i = 0; i + 8 < len; ++i) {
        if (s[i] == '"' && std::memcmp(s + i + 1, "taskId", 6) == 0) {
            keyPos = i;
            break;
        }
    }
    dts::log::Info("[task:handler] config parsed (taskId@{} / {}B)", keyPos, len);
    // 响应：回传 JSON 给 nfoam（进程外，走 DDS；反向 32K 通路同样压）
    if (DtsMw() != nullptr) {
        DtsMw()->publish_external(detmw::endpoint{SESSION_TYPE_DTS, SESSION_INST_TASK,
                                                  MSG_ID_TASK_RESPONSE},
                                  static_cast<const uint8_t*>(data), len);
    }
}

}  // namespace

// ------------------------------------------------------------------
// 业务组「调度」（sessionInst="task"）—— 组内消息表：msgId → 处理函数。
// 新增业务流程 = 在此加一行（msgId 组内唯一，重复由 static_assert 编译期拦截）。
// ------------------------------------------------------------------
constexpr MsgHandler kTaskHandlers[] = {
    {MSG_ID_TASK_ACTIVE, OnTaskActive, "task_active"},  // 调度 -> task：任务激活
    {MSG_ID_TASK_CONFIG, OnTaskConfig, "task_config"},   // nfoam -> task：配置变更（32K JSON）
};
static_assert(MsgIdsUnique(kTaskHandlers, HandlerCount(kTaskHandlers)),
              "task 业务组消息表 msgId 重复：msgId 在 (sessionType, sessionInst) 内必须唯一");

// ------------------------------------------------------------------
// 线程业务组登记：sessionType 固定 "DTS"（static_assert 校验）；sessionInst = 业务组。
// 同一线程扩展新业务组 = 在此数组加一项（如 {SESSION_TYPE_DTS, "cfg", kCfgHandlers, N}），
// 组间 msgId 可独立编号，互不干扰。
// ------------------------------------------------------------------
constexpr SessionMsgTable kTaskGroups[] = {
    {SESSION_TYPE_DTS, SESSION_INST_TASK, kTaskHandlers, HandlerCount(kTaskHandlers)},
};
static_assert(SessionGroupsValid(kTaskGroups, SessionGroupCount(kTaskGroups)),
              "task 业务组登记非法：sessionType 必须全部相同且非空，sessionInst 必须唯一");

const SessionMsgTable* TaskSessionGroups(size_t* count) {
    *count = SessionGroupCount(kTaskGroups);
    return kTaskGroups;
}

void TaskMsgHandlerDispatch(ThreadStatus status, const char* sessionInst, uint32_t msgId,
                            void* msg, uint32_t len) {
    (void)status;
    // 线程本地消息（不属任何业务组）
    if (msgId == MSG_ID_TIMER) {
        return;  // 本线程无定时业务
    }
    // 分发 = 按 sessionInst 选业务组 → 组内按 msgId 查表
    const SessionMsgTable* g =
        FindSessionTable(kTaskGroups, SessionGroupCount(kTaskGroups), sessionInst);
    if (g != nullptr && MsgTable(g->entries, g->count).Dispatch(msgId, msg, len)) {
        return;
    }
    dts::log::Debug("[task:handler] unhandled session={} msgId={:#x}",
                    sessionInst != nullptr ? sessionInst : "-", msgId);
}

}  // namespace dts
