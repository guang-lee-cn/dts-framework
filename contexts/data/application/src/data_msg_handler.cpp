#include <spdlog/spdlog.h>

#include "dts_mw.h"
#include "data_msg_handler.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "data_factory.h"
#include "report_cache.h"
#include "tdmap.h"

namespace dts {

namespace {

// data 线程领域对象（线程内独占，无锁）
TdMap g_tdmap;
ReportCache g_report;

// 状态消息由线程体更新线程状态，业务层不处理（MSG_ID_STATUS）

void OnTaskActive(const uint8_t* msg, uint32_t len) {
    if (msg == nullptr || len < sizeof(TaskActiveMsg)) return;
    TaskActiveMsg m;
    std::memcpy(&m, msg, sizeof(m));
    if (m.dataIdCount > 8) return;

    std::vector<uint16_t> ids(m.dataIds, m.dataIds + m.dataIdCount);
    g_tdmap.Track(m.taskId, ids);
}

void OnAgentData(const uint8_t* msg, uint32_t len) {
    if (msg == nullptr || len == 0) return;
    // 模拟业务下限：4 字节拷贝整包（真实工厂切分/匹配/缓存比这重，本模拟是耗时下限）
    // DTS_DATA_SLOW=N：重复 N 遍拷贝，模拟更慢的业务，扫丢包临界点
    const char* slowEnv = std::getenv("DTS_DATA_SLOW");
    const uint32_t slow = slowEnv != nullptr ? static_cast<uint32_t>(std::strtoul(slowEnv, nullptr, 10)) : 1;
    uint32_t copied = 0;
    const uint32_t* src = reinterpret_cast<const uint32_t*>(msg);
    const uint32_t n = len / 4;
    for (uint32_t rep = 0; rep < slow; ++rep) {
        for (uint32_t i = 0; i < n; ++i) {
            volatile uint32_t v = src[i];  // 防止被优化掉，模拟逐 4 字节处理
            (void)v;
            copied += 4;
        }
    }
    DataFeedStat stat = DataFactory::Instance().Feed(const_cast<char*>(reinterpret_cast<const char*>(msg)),
                                                     len, g_tdmap, g_report);
    // 可观测性：data 线程收帧统计（匹配/被跟踪/丢弃）
    spdlog::info("[data:handler] agent data feed copied={} matched={} tracked={} dropped={} cached={}",
                 copied, stat.matchedDataIds, stat.tracked, stat.dropped, stat.cached);
}

}  // namespace

void DataMsgHandlerDispatch(ThreadStatus status, uint32_t msgId, const uint8_t* msg, uint32_t len) {
    (void)status;
    // 平台 pubsub 已路由到本线程，按 msgId 直分（MSG_ID_STATUS 由线程体更新状态）
    if (msgId == MSG_ID_DATA_TASK_ACTIVE) {
        OnTaskActive(msg, len);
        // 数据上报通路：data 线程收 task 激活即回抛（perf 链路：任务下发 -> 数据上报）
        if (DtsMw() != nullptr) {
            DtsMw()->publish_external(detmw::endpoint{SESSION_TYPE_DTS, SESSION_INST_DATA,
                                                      MSG_ID_REPORT},
                                      msg, len);
        }
    } else if (msgId == MSG_ID_AGENT_DATA) {
        OnAgentData(msg, len);
    }
}

void DataMsgHandlerTimerReport() {
    auto blocks = g_report.TakeAll();
    if (blocks.empty() || DtsMw() == nullptr) return;

    // 经 detmw 上报网管 kafka（对齐 itran 回调模型）
    for (const auto& b : blocks) {
        std::vector<uint8_t> wire;
        wire.reserve(sizeof(ReportHeader) + b.payload.size());
        wire.insert(wire.end(), reinterpret_cast<const uint8_t*>(&b.header),
                    reinterpret_cast<const uint8_t*>(&b.header) + sizeof(b.header));
        wire.insert(wire.end(), b.payload.begin(), b.payload.end());
        DtsMw()->publish_external(detmw::endpoint{SESSION_TYPE_DTS, SESSION_INST_DATA,
                                                  MSG_ID_REPORT},
                                  wire.data(), static_cast<uint32_t>(wire.size()));
    }
}

}  // namespace dts
