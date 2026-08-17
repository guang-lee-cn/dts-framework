#include "kafka_mock.h"

#include <chrono>
#include <cstring>
#include <thread>

#include "detmw.h"
#include "dts_def.h"
#include "dts_mw.h"

namespace dts {

namespace {

// 上报回调（msg4：data 线程合并上报出口）→ 计数（integration 断言链路通）
void OnReport(void*, std::unique_ptr<std::vector<uint8_t>> data) {
    KafkaMock::Record(data ? static_cast<uint64_t>(data->size()) : 0);
}

}  // namespace

void KafkaMock::Start() {
    // 模拟网管 kafka：订阅上报通道（msg4），计数统计。
    // 等组合根装配完成（DtsMw 在 Run 装配早期设置）
    for (int i = 0; i < 100 && DtsMw() == nullptr; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (DtsMw() == nullptr) {
        return;
    }
    DtsMw()->subscribe(detmw::endpoint{SESSION_TYPE_DTS, SESSION_INST_DATA, MSG_ID_REPORT},
                       OnReport, nullptr);
}

}  // namespace dts
