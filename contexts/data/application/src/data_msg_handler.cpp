#include "data_msg_handler.h"

#include <cstring>
#include <vector>

#include "data_factory_v2.h"

namespace dts {

namespace {

// MSG_ID_DATA_TASK_ACTIVE：spaMock 握手 TaskRequest → data 标记 currentTask（直通模式单 task）
void OnTaskActive(const uint8_t* msg, uint32_t len) {
    if (msg == nullptr || len < sizeof(TaskRequest)) {
        return;
    }
    TaskRequest req;
    std::memcpy(&req, msg, sizeof(req));
    if (req.status != 1) {
        return;  // 仅处理握手成功（status=1）
    }
    data::DataFactory::Instance().OnTask(req.taskId, {}, 0);
}

// MSG_ID_AGENT_DATA：spa/业务方 rawData 帧（DataHeader{dataType} + 测量对象键 + 数据字段）
void OnAgentData(const uint8_t* msg, uint32_t len) {
    if (msg == nullptr || len < sizeof(uint16_t)) {
        return;
    }
    const uint16_t dataType = *reinterpret_cast<const uint16_t*>(msg);
    data::DataFactory::Instance().Process(msg, len, dataType);
}

}  // namespace

void DataMsgHandlerDispatch(ThreadStatus status, uint32_t msgId, const uint8_t* msg, uint32_t len) {
    (void)status;
    // 平台 pubsub 已路由到本线程，按 msgId 直分（MSG_ID_STATUS 由线程体更新状态）
    if (msgId == MSG_ID_DATA_TASK_ACTIVE) {
        OnTaskActive(msg, len);
    } else if (msgId == MSG_ID_AGENT_DATA) {
        OnAgentData(msg, len);
    } else if (msgId == MSG_ID_TIMER) {
        // ThreadRun 100ms 超时投递：推进工厂 tick（EvictExpired + 1s FlushReports）
        data::DataFactory::Instance().OnTick();
    }
}

}  // namespace dts
