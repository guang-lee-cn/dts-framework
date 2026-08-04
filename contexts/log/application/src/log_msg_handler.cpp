#include <spdlog/spdlog.h>
#include "log_msg_handler.h"

#include <cstdio>

#include "dts_mw.h"

namespace dts {

namespace {

// 状态消息由线程体更新线程状态，业务层不处理（MSG_ID_STATUS）

void OnCollect(const uint8_t* msg, uint32_t len) {
    spdlog::info("[log:handler] collect msg received ({} bytes), ack", len);
    // log 线程 pub 能力：收到采集即回抛（perf：log 段收发验证）
    if (DtsMw() != nullptr && msg != nullptr) {
        DtsMw()->publish_external(detmw::endpoint{SESSION_TYPE_DTS, SESSION_INST_LOG,
                                                  MSG_ID_LOG_REPORT},
                                  msg, len);
    }
}

}  // namespace

void LogMsgHandlerDispatch(ThreadStatus status, uint32_t msgId, const uint8_t* msg, uint32_t len) {
    (void)status;
    // 平台 pubsub 已路由到本线程，按 msgId 直分（MSG_ID_STATUS 由线程体更新状态）
    if (msgId == MSG_ID_LOG_COLLECT) {
        OnCollect(msg, len);
    }
}

}  // namespace dts
