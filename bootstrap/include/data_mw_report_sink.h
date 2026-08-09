#pragma once

#include "data_factory_v2.h"
#include "dts_def.h"
#include "dts_mw.h"

namespace dts {

// data 上报出口适配：domain ReportSink 抽象 → detmw publish_external(REPORT)。
// 归属组合根（与 DtsMwSet/OnRouteMsg 同属基础设施适配），domain 零技术依赖。
class DataMwReportSink : public data::ReportSink {
public:
    void Publish(const void* data, uint32_t len) override {
        if (DtsMw() == nullptr || data == nullptr || len == 0) {
            return;
        }
        DtsMw()->publish_external(
            detmw::endpoint{SESSION_TYPE_DTS, SESSION_INST_DATA, MSG_ID_REPORT},
            static_cast<const uint8_t*>(data), len);
    }
};

}  // namespace dts
