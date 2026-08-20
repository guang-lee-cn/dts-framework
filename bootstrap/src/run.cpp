#include "run.h"

#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "console.h"
#include "ctl.h"
#include "data_factory_v2.h"
#include "data_mem_manager.h"
#include "data_mw_report_sink.h"
#include "data_msg_handler.h"
#include "detmw.h"
#include "dts_data_entry.h"
#include "dts_def.h"
#include "dts_log_entry.h"
#include "dts_mw.h"
#include "dts_task_entry.h"
#include "dts_thread.h"
#include "log.h"
#include "log_msg_handler.h"
#include "platform_sched_defs.h"
#include "task_msg_handler.h"
#include "thread_api.h"

// 生成的路由头（可执行按进程 include 各自生成目录，见 CMake）
#include "task_routes.h"
#include "data_routes.h"
#include "log_routes.h"
#include "control_routes.h"

namespace dts {

namespace {

// ---- 进程生命周期停止信号：Run 常驻等待，Stop（信号回调/测试线程）请求停止 ----
// 一次性事件：Request 后 Wait 立即返回；不防重入（Request 幂等）
class StopSignal {
public:
    // 请求停止：置位 + 唤醒等待者（任意线程安全）
    void Request() {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_stopped = true;
        }
        m_cv.notify_all();
    }

    // 阻塞等待停止请求（主线程常驻）；返回即收到停止
    void Wait() {
        std::unique_lock<std::mutex> lk(m_mutex);
        m_cv.wait(lk, [this] { return m_stopped; });
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_stopped = false;
};

// ---- 业务线程：纯线程（上下文 + detsched 句柄），只管"跑"，不持有订阅 ----
// 线程名由 ctx.Init 与 detsched 各自持有，此处不冗余存储
struct Worker {
    ThreadCtx ctx;
    detsched::ThreadHandle h = nullptr;
};

// ---- 订阅：detmw 端点 + 目标线程 mailbox。独立概念，Process 统一持有 ----
struct Subscription {
    detmw::endpoint ep;
    ThreadCtx* thread;  // 目标线程 mailbox（业务订阅）；DDS 控制通道不填（OnRouteCtl 直投 control 队列）
};

// console socket 路径：cfg 文件名派生（cpf-dts.json -> /tmp/dts-cpf-dts.sock），多实例不冲突
std::string MakeConsoleSockPath(const char* cfg_path) {
    std::string name = cfg_path;
    const size_t slash = name.find_last_of('/');
    if (slash != std::string::npos) {
        name = name.substr(slash + 1);
    }
    const size_t dot = name.rfind(".json");
    if (dot != std::string::npos) {
        name = name.substr(0, dot);
    }
    return "/tmp/dts-" + name + ".sock";
}

// detmw 回调：消息 -> 目标线程 mailbox（移所有权零拷贝，mailbox 自身线程安全）。
// 盖章 sessionInst（业务组）：借指针指向 Subscription 端点串（订阅生命周期 ≥ 在途消息），
// 线程分发按组选表（msg_table.h FindSessionTable）
void OnRouteMsg(void* userCtx, std::unique_ptr<std::vector<uint8_t>> data) {
    auto* sub = static_cast<Subscription*>(userCtx);
    if (sub == nullptr || sub->thread == nullptr) {
        dts::log::Warn("[Run] route ctx invalid");
        return;
    }
    sub->thread->m_mailbox.Send(sub->ep.msg_id, std::move(data), sub->ep.session_inst.c_str());
}

// DDS 控制通道回调（P1-3，D2/R5）：网管远程命令 -> control 命令队列。
// 不走业务线程 mailbox（D5：运维不进业务线程），执行收敛 control 线程 ctl::Execute（D3）；
// 载荷即命令行文本（console 兼容），control 执行完经 DTS.oam 发布响应（console.cpp）
void OnRouteCtl(void*, std::unique_ptr<std::vector<uint8_t>> data) {
    control_submit_dds(std::move(data));
}

// ---- 控制面：get_handlers —— 查看三级路由（sessionType 固定 · sessionInst 业务组 → msgId 表）----
// 业务线程组登记（msg_handler.cpp 表驱动，新增业务组/消息流自动可见，无需改命令）
using GroupsFn = const SessionMsgTable* (*)(size_t*);
const GroupsFn kGroupFns[] = {TaskSessionGroups, DataSessionGroups, LogSessionGroups};

int CmdGetHandlers(const std::vector<std::string>& args, std::string& out) {
    // 过滤：get_handlers [task|data|log]（空 = 全部线程的全部业务组）
    const std::string want = args.empty() ? "" : args[0];
    for (GroupsFn fn : kGroupFns) {
        size_t groupN = 0;
        const SessionMsgTable* groups = fn(&groupN);
        for (size_t gi = 0; gi < groupN; ++gi) {
            const SessionMsgTable& g = groups[gi];
            if (g.sessionInst == nullptr) {
                continue;
            }
            if (!want.empty() && want != g.sessionInst) {
                continue;
            }
            out += std::string(g.sessionType) + "." + g.sessionInst + " (" +
                   std::to_string(g.count) + " msgs)\n";
            const MsgTable table(g.entries, g.count);
            for (size_t i = 0; i < table.Count(); ++i) {
                const MsgHandler& h = table.At(i);
                char line[64];
                std::snprintf(line, sizeof(line), "  0x%04X  %s\n", h.msgId, h.name);
                out += line;
            }
        }
    }
    return 0;
}

// ---- 控制面：get_data_stats —— data 线程处理统计（帧耗时预算 + 丢弃计数）----
// 确定性预算口径：data 线程内单帧处理（出 mailbox → 处理完成含业务代码），
// 3ms 预警 / 5ms 告警（默认，可配置：set_data_budget / DTS_DATA_BUDGET_*_US）；丢帧/丢切片计数不静默
int CmdGetDataStats(const std::vector<std::string>&, std::string& out) {
    const data::DataStatsSnapshot s = data::DataFactory::Instance().Stats();
    char line[512];
    std::snprintf(line, sizeof(line),
                  "frames=%llu max_us=%llu warn=%llu alarm=%llu ticks=%u "
                  "budget_warn_us=%llu budget_alarm_us=%llu\n"
                  "drop_pool_full=%llu drop_slice_overcap=%llu key_fail=%llu\n",
                  static_cast<unsigned long long>(s.frames),
                  static_cast<unsigned long long>(s.frameMaxUs),
                  static_cast<unsigned long long>(s.frameBudgetWarn),
                  static_cast<unsigned long long>(s.frameBudgetAlarm), s.ticks,
                  static_cast<unsigned long long>(s.budgetWarnUs),
                  static_cast<unsigned long long>(s.budgetAlarmUs),
                  static_cast<unsigned long long>(s.dropPoolFull),
                  static_cast<unsigned long long>(s.dropSliceOverCap),
                  static_cast<unsigned long long>(s.keyFail));
    out += line;
    return 0;
}

// 运行时调整 data 帧耗时预算阈值（按硬件档位/工况调；warn ≤ alarm）
int CmdSetDataBudget(const std::vector<std::string>& args, std::string& out) {
    if (args.size() < 2) {
        out = "usage: set_data_budget <warn_us> <alarm_us>";
        return 1;
    }
    const uint64_t warn = std::strtoull(args[0].c_str(), nullptr, 10);
    const uint64_t alarm = std::strtoull(args[1].c_str(), nullptr, 10);
    if (data::DataFactory::Instance().SetBudget(warn, alarm) != 0) {
        out = "invalid budget (warn must be <= alarm and both > 0)";
        return 1;
    }
    out = "data budget -> warn=" + std::to_string(warn) + "us alarm=" + std::to_string(alarm) +
          "us";
    return 0;
}

// ---- 组合根：通信站点 + 业务线程 + 订阅集合 + 生命周期停止信号，统一装配 ----
struct Process {
    StopSignal stop;  // 生命周期停止信号：外部 RequestStop → WaitStop 返回 → Stop() 下电
    std::unique_ptr<detmw::Communicator> comm;
    std::unique_ptr<data::ReportSink> dataReportSink;  // data 上报出口（DtsMw 适配）
    Worker task;
    Worker data;
    Worker log;
    std::vector<std::unique_ptr<Subscription>> subs;  // 全部订阅（注册在 comm 上）

    // 请求停止（外部：信号回调 / 测试线程）
    void RequestStop() { stop.Request(); }
    // 阻塞等待停止请求（Run 主线程常驻）；返回即收到停止
    void WaitStop() { stop.Wait(); }

    // 装配：声明业务域 -> 建线程 -> 注册订阅 -> console/control 运维面。
    // 返回 false = 致命装配失败（域/线程）；console/control 失败仅降级（进程仍可跑业务）
    bool Start(const char* console_sock) {
        bool ok = true;
        if (!detsched::DeclareDomain(detsched::SchedPrio::Dts::DATA_PRIO)) {
            dts::log::Error("[Run] DeclareDomain failed");
            ok = false;
        }
        ok = StartWorker(task, "task", TaskEntry, detsched::SchedPrio::Dts::TASK_PRIO) && ok;
        ok = StartWorker(data, "data", DataEntry, detsched::SchedPrio::Dts::DATA_PRIO) && ok;
        ok = StartWorker(log, "log", LogEntry, detsched::SchedPrio::Dts::LOG_PRIO) && ok;

        RegisterSubRoutes(task.ctx, SESSION_TYPE_DTS, kTaskSubRoutes, kTaskSubRouteCount);
        RegisterSubRoutes(data.ctx, SESSION_TYPE_DTS, kDataSubRoutes, kDataSubRouteCount);
        RegisterSubRoutes(log.ctx, SESSION_TYPE_DTS, kLogSubRoutes, kLogSubRouteCount);

        if (dts::console_start(console_sock) != 0) {
            dts::log::Error("[Run] console start failed (sock={})", console_sock);
        }
        // DDS 控制通道（P1-3，D2/R5）：control 线程在才开；不在则降级（无远程控制，本地业务不受影响）
        if (dts::control_start() == 0) {
            RegisterCtlRoutes();
        } else {
            dts::log::Error("[Run] control start failed (DDS control channel disabled)");
        }
        // 命令表登记：业务线程消息表展示 + data 处理统计（bootstrap 组装，contexts 无反向依赖）
        dts::ctl::CommandRegistry::Instance().Register(
            {"get_handlers", "get_handlers [task|data|log] 查看线程消息表（msgId→处理函数）",
             CmdGetHandlers});
        dts::ctl::CommandRegistry::Instance().Register(
            {"get_data_stats", "get_data_stats 查看 data 线程处理统计（帧耗时预算/丢弃计数）",
             CmdGetDataStats});
        dts::ctl::CommandRegistry::Instance().Register(
            {"set_data_budget", "set_data_budget <warn_us> <alarm_us> 调整 data 帧耗时预算阈值",
             CmdSetDataBudget});
        return ok;
    }

    // 下电：先停 console/control（反序），再停业务线程，最后销毁 detmw。
    // 前提：FastDDS delete_participant 阻塞等待接收线程退出，在途 OnRouteMsg 回调必已结束，
    // 故 comm.reset() 后 subs.clear() 无 use-after-free（换传输实现需重新验证该假设）
    void Stop() {
        dts::control_stop();
        dts::console_stop();
        StopWorker(task);
        StopWorker(data);
        StopWorker(log);
        DtsMwSet(nullptr);
        comm.reset();
        subs.clear();
    }

private:
    bool StartWorker(Worker& w, const char* name, EntryFn entry, int prio) {
        w.ctx.Init(name, entry);
        w.h = detsched::CreateThread(name, prio, ThreadEntry, &w.ctx);
        if (w.h == nullptr) {
            dts::log::Error("[Run] thread create failed: {}", name);
            return false;
        }
        return true;
    }

    void StopWorker(Worker& w) {
        if (w.h == nullptr) return;  // 装配失败/未建的线程无需下电
        w.ctx.RequestStop();
        detsched::DestroyThread(w.h);
    }

    // 订阅登记：生成路由头 Sub 表（可能为空表——gen_detmw.py 恒定生成四线程头）
    template <typename RouteArray>
    void RegisterSubRoutes(ThreadCtx& thread, const char* session_type,
                           const RouteArray* routes, size_t count) {
        for (size_t i = 0; i < count; i++) {
            auto sub = std::make_unique<Subscription>();
            sub->ep = detmw::endpoint{session_type, routes[i].sessionInst, routes[i].msgId};
            sub->thread = &thread;
            if (comm->subscribe(sub->ep, OnRouteMsg, sub.get()) == 0) {
                subs.push_back(std::move(sub));
            } else {
                dts::log::Error("[Run] subscribe failed: {}", sub->ep.ToString());
            }
        }
    }

    // DDS 控制通道订阅（config thread="control" 的 Sub 表，可能为空表）：OnRouteCtl 直投
    // control 命令队列（不绑业务线程 mailbox，D5）
    void RegisterCtlRoutes() {
        for (size_t i = 0; i < kControlSubRouteCount; i++) {
            auto sub = std::make_unique<Subscription>();
            sub->ep = detmw::endpoint{SESSION_TYPE_DTS, kControlSubRoutes[i].sessionInst,
                                      kControlSubRoutes[i].msgId};
            if (comm->subscribe(sub->ep, OnRouteCtl, sub.get()) == 0) {
                subs.push_back(std::move(sub));
            } else {
                dts::log::Error("[Run] control subscribe failed: {}", sub->ep.ToString());
            }
        }
    }
};

Process& State() {
    static Process s;
    return s;
}

}  // namespace

void Stop() {
    State().RequestStop();
}

int Run(const char* cfg_path) {
    // 1. 日志：先于一切，失败路径也要有日志出口
    dts::log::Init();
    if (cfg_path == nullptr) {
        dts::log::Error("[Run] cfg_path is null");
        return 1;
    }

    // 2. 通信站点（detmw v2：加载配置 + 建 participant + 预建 writer）
    auto& p = State();
    p.comm = std::make_unique<detmw::Communicator>(cfg_path);
    if (!p.comm->good()) {
        dts::log::Error("[Run] detmw init failed (cfg={})", cfg_path);
        p.Stop();           // comm 析构（transport 未起，安全）
        dts::log::Shutdown();
        return 2;
    }
    DtsMwSet(p.comm.get());

    // 2.5 data 子系统：内存池初始化 + extractor 注册（强制链接）+ 上报出口注入（DtsMw 适配）
    dts::data::DataMemManager::Instance().Init();
    dts::data::InitExtractors();
    p.dataReportSink = std::make_unique<DataMwReportSink>();
    dts::data::DataFactory::Instance().SetReportSink(p.dataReportSink.get());

    // 3. 业务线程 + 订阅 + console/control 装配
    if (!p.Start(MakeConsoleSockPath(cfg_path).c_str())) {
        dts::log::Error("[Run] thread assembly failed");
        p.Stop();
        dts::log::Shutdown();
        return 3;
    }

    dts::log::Info("[Run] up (cfg={})", cfg_path);

    // 4. 常驻运行：阻塞等待停止请求（Stop 唤醒）
    p.WaitStop();

    // 5. 反序下电
    p.Stop();
    dts::log::Shutdown();  // 最后停异步日志线程池，刷空队列
    return 0;
}

}  // namespace dts
