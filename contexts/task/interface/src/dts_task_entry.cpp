#include "dts_task_entry.h"

#include "task_msg_handler.h"

namespace dts {

void TaskEntry(ThreadStatus status, const char* sessionInst, uint32_t msgId, void* msg,
               uint32_t len) {
    TaskMsgHandlerDispatch(status, sessionInst, msgId, msg, len);
}

}  // namespace dts
