#include "dts_task_entry.h"

#include "task_msg_handler.h"

namespace dts {

void TaskEntry(ThreadStatus status, uint32_t msgId, const uint8_t* msg, uint32_t len) {
    TaskMsgHandlerDispatch(status, msgId, msg, len);
}

}  // namespace dts
