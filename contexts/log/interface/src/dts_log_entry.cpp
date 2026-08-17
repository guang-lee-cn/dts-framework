#include "dts_log_entry.h"

#include "log_msg_handler.h"

namespace dts {

void LogEntry(ThreadStatus status, const char* sessionInst, uint32_t msgId, void* msg,
              uint32_t len) {
    LogMsgHandlerDispatch(status, sessionInst, msgId, msg, len);
}

}  // namespace dts
