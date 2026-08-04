#include "dts_startup.h"

#include <memory>

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
ThreadCtx g_taskCtx;
ThreadCtx g_dataCtx;
ThreadCtx g_logCtx;
detsched::ThreadHandle g_taskH = nullptr;
detsched::ThreadHandle g_dataH = nullptr;
detsched::ThreadHandle g_logH = nullptr;
detmw_handle* g_mw = nullptr;

void InitLog() {
    // 异步日志：队列满丢最旧（overrun_oldest），实时线程不被日志 I/O 阻塞
    spdlog::init_thread_pool(8192, 1);
    auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto logger = std::make_shared<spdlog::async_logger>(
        "dts", sink, spdlog::thread_pool(), spdlog::async_overflow_policy::overrun_oldest);
    spdlog::set_default_logger(logger);
    spdlog::set_level(spdlog::level::info);
}

// detmw 路由订阅：消息 -> 线程 mailbox（detmw_recv_fn 是函数指针，用 user_ctx 传路由上下文）
struct RouteCtx {
    ThreadCtx* thread;
    uint32_t msgId;
};

void OnRouteMsg(void* userCtx, const uint8_t* data, uint32_t len) {
    auto* rc = static_cast<RouteCtx*>(userCtx);
    rc->thread->m_mailbox.Send(rc->msgId, data, len);
}

void RouteToThread(ThreadCtx& ctx, const char* inst, uint32_t msgId) {
    auto* rc = new RouteCtx{&ctx, msgId};  // 进程常驻，不回收
    detmw_subscribe(g_mw, SESSION_TYPE_DTS, inst, msgId, OnRouteMsg, rc);
}

// 按线程遍历生成 Sub 路由表注册订阅（替代手写 MSG_ID）
template <typename RouteArray, size_t N>
void RegisterSubRoutes(ThreadCtx& ctx, const RouteArray (&routes)[N]) {
    for (size_t i = 0; i < N; i++) {
        RouteToThread(ctx, routes[i].sessionInst, routes[i].msgId);
    }
}
}  // namespace

int dts_startup(const char* cfg_path) {
    if (cfg_path == nullptr) {
        spdlog::error("[dts_startup] cfg_path is null");
        return 1;
    }

    // 1. 日志初始化（异步 spdlog）
    InitLog();

    // 2. 通信中间件初始化（detmw，加载分进程生成配置 JSON + 静态发现）
    g_mw = detmw_init(cfg_path);
    if (g_mw == nullptr) {
        spdlog::error("[dts_startup] detmw init failed");
        return 1;
    }
    DtsMwSet(g_mw);

    // 3. 各线程消息处理初始化

    // 4. 声明业务域（detsched 代码层校验前提）
    detsched::DeclareDomain(detsched::SchedPrio::Dts::DATA_PRIO);

    // 5. detmw 静态路由：session + msgId -> 线程 mailbox（遍历生成路由表，替代手写 MSG_ID）
    RegisterSubRoutes(g_taskCtx, kTaskSubRoutes);
    RegisterSubRoutes(g_dataCtx, kDataSubRoutes);
    RegisterSubRoutes(g_logCtx, kLogSubRoutes);

    // 6. 组合根装配：ThreadCtx（业务）+ detsched（线程引擎）
    g_taskCtx.Init("task", TaskEntry);
    g_dataCtx.Init("data", DataEntry);
    g_logCtx.Init("log", LogEntry);

    g_taskH = detsched::CreateThread("task", detsched::SchedPrio::Dts::TASK_PRIO, ThreadEntry, &g_taskCtx);
    g_dataH = detsched::CreateThread("data", detsched::SchedPrio::Dts::DATA_PRIO, ThreadEntry, &g_dataCtx);
    g_logH = detsched::CreateThread("log", detsched::SchedPrio::Dts::LOG_PRIO, ThreadEntry, &g_logCtx);

    spdlog::info("[dts_startup] up (cfg={})", cfg_path);
    return 0;
}

void dts_startup_shutdown() {
    // 业务循环退出 -> detsched 回收
    g_taskCtx.RequestStop();
    detsched::DestroyThread(g_taskH);
    g_dataCtx.RequestStop();
    detsched::DestroyThread(g_dataH);
    g_logCtx.RequestStop();
    detsched::DestroyThread(g_logH);

    detmw_destroy(g_mw);
    g_mw = nullptr;
    DtsMwSet(nullptr);
}

}  // namespace dts
