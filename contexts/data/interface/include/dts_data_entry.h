#pragma once

#include <cstdint>

#include "dts_def.h"

namespace dts {

// data 线程接入层入口：status/sessionInst(业务组)/msgId 已知，几行转发到 msg_handler
// （msg 非 const，可原地处理）
void DataEntry(ThreadStatus status, const char* sessionInst, uint32_t msgId, void* msg,
               uint32_t len);

}  // namespace dts
