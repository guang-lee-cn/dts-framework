#pragma once

#include <cstdint>

#include "dts_def.h"

namespace dts {

// task 线程接入层入口：status 已知、msgId 已知，几行转发到 msg_handler
void TaskEntry(ThreadStatus status, uint32_t msgId, const uint8_t* msg, uint32_t len);

}  // namespace dts
