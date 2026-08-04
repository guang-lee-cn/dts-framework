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

// ---- 订阅：detmw 端点 + 所属线程 mailbox ----
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

// ---- 业务线程：上下文 + detsched 句柄 + 订阅集合（自包含）----
struct Worker {
    std::string name;
    ThreadCtx ctx;
    detsched::ThreadHandle h = nullptr;
    std::vector<std::unique_ptr<Subscription>> subs;  // 本线程负责的订阅

    template <typename RouteArray, size_t N>
    void Start(detmw::Communicator& comm, const char* threadName, EntryFn entry, int prio,
               const char* session_type, const RouteArray (&routes)[N]) {
        name = threadName;
        ctx.Init(name, entry);
        for (size_t i = 0; i < N; i++) {
            auto sub = std::make_unique<Subscription>();
            sub->ep = detmw::endpoint{session_type, routes[i].sessionInst, routes[i].msgId};
            sub->thread = &ctx;
            if (comm.subscribe(sub->ep, OnRouteMsg, sub.get()) == 0) {
                subs.push_back(std::move(sub));
            } else {
                spdlog::error("[Run] {} subscribe failed: {}", name, sub->ep.ToString());
            }
        }
        h = detsched::CreateThread(name, prio, ThreadEntry, &ctx);
    }

    void Stop() {
        ctx.RequestStop();
        detsched::DestroyThread(h);
        subs.clear();
    }
};

// ---- 组合根：通信站点 + 三个业务线程 ----
struct Process {
    std::unique_ptr<detmw::Communicator> comm;
    Worker task;
    Worker data;
    Worker log;

    void Start() {
        // 声明业务域（detsched 代码层校验前提）
        detsched::DeclareDomain(detsched::SchedPrio::Dts::DATA_PRIO);
        // 业务线程：订阅接入 comm + 创建线程（Worker 自包含，一步装配）
        task.Start(*comm, "task", TaskEntry, detsched::SchedPrio::Dts::TASK_PRIO, SESSION_TYPE_DTS,
                   kTaskSubRoutes);
        data.Start(*comm, "data", DataEntry, detsched::SchedPrio::Dts::DATA_PRIO, SESSION_TYPE_DTS,
                   kDataSubRoutes);
        log.Start(*comm, "log", LogEntry, detsched::SchedPrio::Dts::LOG_PRIO, SESSION_TYPE_DTS,
                  kLogSubRoutes);
    }

    void Stop() {
        // 反序：先停业务线程，再销毁 detmw
        task.Stop();
        data.Stop();
        log.Stop();
        DtsMwSet(nullptr);
        comm.reset();
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

    // 3. 业务线程装配
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
