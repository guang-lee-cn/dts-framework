#include "dts_log_entry.h"

#include "log_msg_handler.h"

namespace dts {

void LogEntry(ThreadStatus status, uint32_t msgId, const uint8_t* msg, uint32_t len) {
    LogMsgHandlerDispatch(status, msgId, msg, len);
}

}  // namespace dts
