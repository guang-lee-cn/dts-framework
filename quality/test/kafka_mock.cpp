#include "kafka_mock.h"

namespace dts {

void KafkaMock::Start() {
    // TODO(platform): 模拟网管 kafka，订阅平台 pubsub 上报通道
    //   pubsub.Subscribe({SESSION_TYPE_DTS, SESSION_INST_DATA, MSG_ID_REPORT},
    //                     [](data, len) { 解析 ReportHeader 并打印/统计 });
}

}  // namespace dts
