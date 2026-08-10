#include "run.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "console.h"
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
    ThreadCtx* thread;  // 目标线程 mailbox（回调投递目标）
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

// detmw 回调：消息 -> 目标线程 mailbox（移所有权零拷贝，mailbox 自身线程安全）
void OnRouteMsg(void* userCtx, std::unique_ptr<std::vector<uint8_t>> data) {
    auto* sub = static_cast<Subscription*>(userCtx);
    if (sub == nullptr || sub->thread == nullptr) {
        dts::log::Warn("[Run] route ctx invalid");
        return;
    }
    sub->thread->m_mailbox.Send(sub->ep.msg_id, std::move(data));
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

        RegisterSubRoutes(task.ctx, SESSION_TYPE_DTS, kTaskSubRoutes);
        RegisterSubRoutes(data.ctx, SESSION_TYPE_DTS, kDataSubRoutes);
        RegisterSubRoutes(log.ctx, SESSION_TYPE_DTS, kLogSubRoutes);

        if (dts::console_start(console_sock) != 0) {
            dts::log::Error("[Run] console start failed (sock={})", console_sock);
        }
        if (dts::control_start() != 0) {
            dts::log::Error("[Run] control start failed");
        }
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

    template <typename RouteArray, size_t N>
    void RegisterSubRoutes(ThreadCtx& thread, const char* session_type,
                           const RouteArray (&routes)[N]) {
        for (size_t i = 0; i < N; i++) {
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
