#include "data_msg_handler.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "data_factory_v2.h"
#include "dts_mw.h"
#include "log.h"

namespace dts {

namespace {

// ------------------------------------------------------------------
// 第三层路由处理函数：组内 msgId 命中后由消息表调用，签名 (void* data, uint32_t len)。
// 一个函数 = 一个消息流；状态消息（MSG_ID_STATUS）由线程体消费，不在此层。
// ------------------------------------------------------------------

// MSG_ID_DATA_TASK_ACTIVE：spaMock 握手 TaskRequest → data 标记 currentTask（直通模式单 task）
void OnTaskActive(void* data, uint32_t len) {
    if (data == nullptr || len < sizeof(TaskRequest)) {
        return;
    }
    TaskRequest req;
    std::memcpy(&req, data, sizeof(req));
    if (req.status != 1) {
        return;  // 仅处理握手成功（status=1）
    }
    data::DataFactory::Instance().OnTask(req.taskId, {}, 0);
}

// MSG_ID_AGENT_DATA：spa/业务方 rawData 帧（DataHeader{dataType} + 测量对象键 + 数据字段）
void OnAgentData(void* data, uint32_t len) {
    if (data == nullptr || len < sizeof(uint16_t)) {
        return;
    }
    uint16_t dataType = 0;
    std::memcpy(&dataType, data, sizeof(dataType));  // 按字节拷贝（载荷可非对齐）
    data::DataFactory::Instance().Process(data, len, dataType);
}

// MSG_ID_TIMER（线程本地，不属业务组）：ThreadRun 100ms 超时投递 → 推进工厂 tick
void OnTick(void*, uint32_t) {
    data::DataFactory::Instance().OnTick();
}

// ------------------------------------------------------------------
// 运维业务组（sessionInst="oam"，挂在 data 线程上）：网管拉取指标。
// 响应格式：可读文本（key=value 行）；网管对接时可换二进制/JSON 协议（MsgHandlerFn 不变）。
// ------------------------------------------------------------------
void OnOamStatsReq(void*, uint32_t) {
    dts::log::Info("[data:oam] stats req received");
    const data::DataStatsSnapshot s = data::DataFactory::Instance().Stats();
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "frames=%llu\nmax_us=%llu\nwarn=%llu\nalarm=%llu\nticks=%u\n"
                  "drop_pool_full=%llu\ndrop_slice_overcap=%llu\nkey_fail=%llu\n"
                  "budget_warn_us=%llu\nbudget_alarm_us=%llu\n",
                  static_cast<unsigned long long>(s.frames),
                  static_cast<unsigned long long>(s.frameMaxUs),
                  static_cast<unsigned long long>(s.frameBudgetWarn),
                  static_cast<unsigned long long>(s.frameBudgetAlarm), s.ticks,
                  static_cast<unsigned long long>(s.dropPoolFull),
                  static_cast<unsigned long long>(s.dropSliceOverCap),
                  static_cast<unsigned long long>(s.keyFail),
                  static_cast<unsigned long long>(s.budgetWarnUs),
                  static_cast<unsigned long long>(s.budgetAlarmUs));
    if (DtsMw() != nullptr) {
        DtsMw()->publish_external(detmw::endpoint{SESSION_TYPE_DTS, SESSION_INST_OAM,
                                                  MSG_ID_OAM_STATS_RESP},
                                  reinterpret_cast<const uint8_t*>(buf),
                                  static_cast<uint32_t>(std::strlen(buf)));
    }
}

}  // namespace

// ------------------------------------------------------------------
// 业务组「数据」（sessionInst="data"）—— 组内消息表：msgId → 处理函数。
// 新增业务流程 = 在此加一行（msgId 组内唯一，重复由 static_assert 编译期拦截）。
// ------------------------------------------------------------------
constexpr MsgHandler kDataHandlers[] = {
    {MSG_ID_DATA_TASK_ACTIVE, OnTaskActive, "data_task_active"},  // task/spaMock -> data：握手建任务
    {MSG_ID_AGENT_DATA, OnAgentData, "agent_data"},               // agent -> data：rawData 帧
};
static_assert(MsgIdsUnique(kDataHandlers, HandlerCount(kDataHandlers)),
              "data 业务组消息表 msgId 重复：msgId 在 (sessionType, sessionInst) 内必须唯一");

// ------------------------------------------------------------------
// 业务组「运维」（sessionInst="oam"，挂在 data 线程）—— 指标拉取（P1-2 指标出口）。
// 组间 msgId 独立编号（0x000A/0x000B 与 data 组 0x0002/0x0003 互不冲突）。
// ------------------------------------------------------------------
constexpr MsgHandler kOamHandlers[] = {
    {MSG_ID_OAM_STATS_REQ, OnOamStatsReq, "oam_stats_req"},  // 网管 -> data：拉取处理统计
};
static_assert(MsgIdsUnique(kOamHandlers, HandlerCount(kOamHandlers)),
              "oam 业务组消息表 msgId 重复：msgId 在 (sessionType, sessionInst) 内必须唯一");

// ------------------------------------------------------------------
// 线程业务组登记：sessionType 固定 "DTS"（static_assert 校验）；sessionInst = 业务组。
// 同一线程扩展新业务组 = 在此数组加一项（本例：data 线程挂「数据」+「运维」两组），
// 组间 msgId 可独立编号，互不干扰。
// ------------------------------------------------------------------
constexpr SessionMsgTable kDataGroups[] = {
    {SESSION_TYPE_DTS, SESSION_INST_DATA, kDataHandlers, HandlerCount(kDataHandlers)},
    {SESSION_TYPE_DTS, SESSION_INST_OAM, kOamHandlers, HandlerCount(kOamHandlers)},
};
static_assert(SessionGroupsValid(kDataGroups, SessionGroupCount(kDataGroups)),
              "data 业务组登记非法：sessionType 必须全部相同且非空，sessionInst 必须唯一");

const SessionMsgTable* DataSessionGroups(size_t* count) {
    *count = SessionGroupCount(kDataGroups);
    return kDataGroups;
}

void DataMsgHandlerDispatch(ThreadStatus status, const char* sessionInst, uint32_t msgId,
                            void* msg, uint32_t len) {
    (void)status;
    // 线程本地消息（不属任何业务组）：定时 tick 驱动工厂（EvictExpired + 节拍）
    if (msgId == MSG_ID_TIMER) {
        OnTick(msg, len);
        return;
    }
    // 分发 = 按 sessionInst 选业务组 → 组内按 msgId 查表
    const SessionMsgTable* g =
        FindSessionTable(kDataGroups, SessionGroupCount(kDataGroups), sessionInst);
    if (g != nullptr && MsgTable(g->entries, g->count).Dispatch(msgId, msg, len)) {
        return;
    }
    dts::log::Debug("[data:handler] unhandled session={} msgId={:#x}",
                    sessionInst != nullptr ? sessionInst : "-", msgId);
}

}  // namespace dts
