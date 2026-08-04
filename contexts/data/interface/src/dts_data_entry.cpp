#include "dts_data_entry.h"

#include "data_msg_handler.h"

namespace dts {

void DataEntry(ThreadStatus status, uint32_t msgId, const uint8_t* msg, uint32_t len) {
    DataMsgHandlerDispatch(status, msgId, msg, len);
}

}  // namespace dts
