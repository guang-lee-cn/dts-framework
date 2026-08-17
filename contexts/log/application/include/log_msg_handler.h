#pragma once

#include <cstddef>
#include <cstdint>

#include "dts_def.h"
#include "msg_table.h"

namespace dts {

// log 线程消息处理：第一层 sessionType（线程内固定）+ 第二层 sessionInst（业务组）
// 已在订阅期完成，本层做第三层路由：按 sessionInst 选业务组 → 组内 msgId 查表
// （kLogGroups，见 log_msg_handler.cpp）。新增业务流程 = 组内消息表加一行。
void LogMsgHandlerDispatch(ThreadStatus status, const char* sessionInst, uint32_t msgId,
                           void* msg, uint32_t len);

// 线程业务组登记：sessionType 固定 "DTS"，sessionInst = 业务组（可多组）。
// 供 bootstrap 统一登记（ctl get_handlers 展示）
const SessionMsgTable* LogSessionGroups(size_t* count);

}  // namespace dts
