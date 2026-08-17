#include "dts_data_entry.h"

#include "data_msg_handler.h"

namespace dts {

void DataEntry(ThreadStatus status, const char* sessionInst, uint32_t msgId, void* msg,
               uint32_t len) {
    DataMsgHandlerDispatch(status, sessionInst, msgId, msg, len);
}

}  // namespace dts
