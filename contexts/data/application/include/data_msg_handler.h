#pragma once

#include <cstdint>

#include "dts_def.h"

namespace dts {

// data 线程消息处理：平台 pubsub 已按 (session, msgId) 路由到本线程，按 msgId 直分
// 含 MSG_ID_TIMER（ThreadRun 100ms 超时）→ DataFactory::OnTick 驱动周期上报
void DataMsgHandlerDispatch(ThreadStatus status, uint32_t msgId, const uint8_t* msg, uint32_t len);

}  // namespace dts
