#pragma once

#include <cstdint>

#include "dts_def.h"

namespace dts {

// task 线程消息处理：平台 pubsub 已按 (session, msgId) 路由到本线程，按 msgId 直分
void TaskMsgHandlerDispatch(ThreadStatus status, uint32_t msgId, const uint8_t* msg, uint32_t len);

}  // namespace dts
