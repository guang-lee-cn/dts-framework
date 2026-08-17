#include "log_msg_handler.h"

#include "dts_mw.h"
#include "log.h"

namespace dts {

namespace {

// ------------------------------------------------------------------
// 第三层路由处理函数：组内 msgId 命中后由消息表调用，签名 (void* data, uint32_t len)。
// 一个函数 = 一个消息流；状态消息（MSG_ID_STATUS）由线程体消费，不在此层。
// ------------------------------------------------------------------

// MSG_ID_LOG_COLLECT：调度 -> log 采集指令；收到即回抛（log 线程 pub 能力验证）
void OnCollect(void* data, uint32_t len) {
    dts::log::Info("[log:handler] collect msg received ({} bytes), ack", len);
    if (DtsMw() != nullptr && data != nullptr) {
        DtsMw()->publish_external(detmw::endpoint{SESSION_TYPE_DTS, SESSION_INST_LOG,
                                                  MSG_ID_LOG_REPORT},
                                  static_cast<const uint8_t*>(data), len);
    }
}

}  // namespace

// ------------------------------------------------------------------
// 业务组「日志」（sessionInst="log"）—— 组内消息表：msgId → 处理函数。
// 新增业务流程 = 在此加一行（msgId 组内唯一，重复由 static_assert 编译期拦截）。
// ------------------------------------------------------------------
constexpr MsgHandler kLogHandlers[] = {
    {MSG_ID_LOG_COLLECT, OnCollect, "log_collect"},  // 调度 -> log：采集指令（回抛 ack）
};
static_assert(MsgIdsUnique(kLogHandlers, HandlerCount(kLogHandlers)),
              "log 业务组消息表 msgId 重复：msgId 在 (sessionType, sessionInst) 内必须唯一");

// ------------------------------------------------------------------
// 线程业务组登记：sessionType 固定 "DTS"（static_assert 校验）；sessionInst = 业务组。
// ------------------------------------------------------------------
constexpr SessionMsgTable kLogGroups[] = {
    {SESSION_TYPE_DTS, SESSION_INST_LOG, kLogHandlers, HandlerCount(kLogHandlers)},
};
static_assert(SessionGroupsValid(kLogGroups, SessionGroupCount(kLogGroups)),
              "log 业务组登记非法：sessionType 必须全部相同且非空，sessionInst 必须唯一");

const SessionMsgTable* LogSessionGroups(size_t* count) {
    *count = SessionGroupCount(kLogGroups);
    return kLogGroups;
}

void LogMsgHandlerDispatch(ThreadStatus status, const char* sessionInst, uint32_t msgId,
                           void* msg, uint32_t len) {
    (void)status;
    // 线程本地消息（不属任何业务组）
    if (msgId == MSG_ID_TIMER) {
        return;  // 本线程无定时业务
    }
    // 分发 = 按 sessionInst 选业务组 → 组内按 msgId 查表
    const SessionMsgTable* g =
        FindSessionTable(kLogGroups, SessionGroupCount(kLogGroups), sessionInst);
    if (g != nullptr && MsgTable(g->entries, g->count).Dispatch(msgId, msg, len)) {
        return;
    }
    dts::log::Debug("[log:handler] unhandled session={} msgId={:#x}",
                    sessionInst != nullptr ? sessionInst : "-", msgId);
}

}  // namespace dts
