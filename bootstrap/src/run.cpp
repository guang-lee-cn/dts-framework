#include "run.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <spdlog/async.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "data_msg_handler.h"
#include "detmw.h"
#include "dts_data_entry.h"
#include "dts_def.h"
#include "dts_log_entry.h"
#include "dts_mw.h"
#include "dts_task_entry.h"
#include "dts_thread.h"
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

// ---- 停止标志：Run 常驻循环等待（cv 唤醒，兼容信号回调与测试线程调用）----
std::mutex g_runMutex;
std::condition_variable g_runCv;
bool g_stopped = false;

// ---- 业务线程：纯线程（上下文 + detsched 句柄），只管"跑"，不持有订阅 ----
struct Worker {
    std::string name;
    ThreadCtx ctx;
    detsched::ThreadHandle h = nullptr;
};

// ---- 订阅：detmw 端点 + 目标线程 mailbox。独立概念，Process 统一持有 ----
struct Subscription {
    detmw::endpoint ep;
    ThreadCtx* thread;  // 目标线程 mailbox（回调投递目标）
};

// detmw 回调：消息 -> 目标线程 mailbox（跨线程投递，mailbox 自身线程安全）
void OnRouteMsg(void* userCtx, const uint8_t* data, uint32_t len) {
    auto* sub = static_cast<Subscription*>(userCtx);
    if (sub == nullptr || sub->thread == nullptr || (data == nullptr && len > 0)) {
        spdlog::warn("[Run] route ctx invalid");
        return;
    }
    sub->thread->m_mailbox.Send(sub->ep.msg_id, data, len);
}

// ---- 组合根：通信站点 + 业务线程 + 订阅集合，统一装配 ----
struct Process {
    std::unique_ptr<detmw::Communicator> comm;
    Worker task;
    Worker data;
    Worker log;
    std::vector<std::unique_ptr<Subscription>> subs;  // 全部订阅（注册在 comm 上）

    // 装配：声明业务域 -> 建线程 -> 注册订阅
    void Start() {
        detsched::DeclareDomain(detsched::SchedPrio::Dts::DATA_PRIO);

        StartWorker(task, "task", TaskEntry, detsched::SchedPrio::Dts::TASK_PRIO);
        StartWorker(data, "data", DataEntry, detsched::SchedPrio::Dts::DATA_PRIO);
        StartWorker(log, "log", LogEntry, detsched::SchedPrio::Dts::LOG_PRIO);

        RegisterSubRoutes(task.ctx, SESSION_TYPE_DTS, kTaskSubRoutes);
        RegisterSubRoutes(data.ctx, SESSION_TYPE_DTS, kDataSubRoutes);
        RegisterSubRoutes(log.ctx, SESSION_TYPE_DTS, kLogSubRoutes);
    }

    // 下电：先停业务线程，再销毁 detmw（订阅随 comm 释放）
    void Stop() {
        StopWorker(task);
        StopWorker(data);
        StopWorker(log);
        DtsMwSet(nullptr);
        comm.reset();
        subs.clear();
    }

private:
    void StartWorker(Worker& w, const char* name, EntryFn entry, int prio) {
        w.name = name;
        w.ctx.Init(name, entry);
        w.h = detsched::CreateThread(name, prio, ThreadEntry, &w.ctx);
    }

    void StopWorker(Worker& w) {
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
                spdlog::error("[Run] subscribe failed: {}", sub->ep.ToString());
            }
        }
    }
};

Process& State() {
    static Process s;
    return s;
}

void InitLog() {
    spdlog::init_thread_pool(8192, 1);
    auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto logger = std::make_shared<spdlog::async_logger>(
        "dts", sink, spdlog::thread_pool(), spdlog::async_overflow_policy::overrun_oldest);
    spdlog::set_default_logger(logger);
    spdlog::set_level(spdlog::level::info);
}

}  // namespace

void Stop() {
    {
        std::lock_guard<std::mutex> lk(g_runMutex);
        g_stopped = true;
    }
    g_runCv.notify_all();
}

int Run(const char* cfg_path) {
    if (cfg_path == nullptr) {
        spdlog::error("[Run] cfg_path is null");
        return 1;
    }

    // 1. 日志
    InitLog();

    // 2. 通信站点（detmw v2：加载配置 + 建 participant + 预建 writer）
    auto& p = State();
    p.comm = std::make_unique<detmw::Communicator>(cfg_path);
    DtsMwSet(p.comm.get());

    // 3. 业务线程 + 订阅装配
    p.Start();

    spdlog::info("[Run] up (cfg={})", cfg_path);

    // 4. 常驻运行：等待 Stop() 置停止标志后退出（cv 唤醒）
    {
        std::unique_lock<std::mutex> lk(g_runMutex);
        g_runCv.wait(lk, [] { return g_stopped; });
    }

    // 5. 反序下电
    p.Stop();
    spdlog::shutdown();  // 最后停异步日志线程池，刷空队列
    return 0;
}

}  // namespace dts
