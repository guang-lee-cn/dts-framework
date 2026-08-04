#include "startup.h"

#include <memory>
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

// detmw 路由订阅：消息 -> 线程 mailbox（detmw_recv_fn 是函数指针，用 user_ctx 传路由上下文）
struct RouteCtx {
    ThreadCtx* thread;
    uint32_t msgId;
};

void OnRouteMsg(void* userCtx, const uint8_t* data, uint32_t len) {
    auto* rc = static_cast<RouteCtx*>(userCtx);
    if (rc == nullptr || rc->thread == nullptr || (data == nullptr && len > 0)) {
        spdlog::warn("[StartUp] route ctx invalid");
        return;
    }
    rc->thread->m_mailbox.Send(rc->msgId, data, len);
}

// 单个业务线程：上下文 + detsched 句柄成对（StartUp 创建 / ShutDown 反序回收）
struct Worker {
    ThreadCtx ctx;
    detsched::ThreadHandle h = nullptr;
};

// 组合根常驻资源（进程生命周期，StartUp 装配 / ShutDown 反序释放）
struct AppState {
    std::unique_ptr<detmw_handle, decltype(&detmw_destroy)> mw{nullptr, &detmw_destroy};
    Worker task;
    Worker data;
    Worker log;
    std::vector<std::unique_ptr<RouteCtx>> routes;  // 订阅上下文，进程常驻但所有权明确
};

AppState& State() {
    static AppState s;
    return s;
}

void InitLog() {
    // 异步日志：队列满丢最旧（overrun_oldest），实时线程不被日志 I/O 阻塞
    spdlog::init_thread_pool(8192, 1);
    auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto logger = std::make_shared<spdlog::async_logger>(
        "dts", sink, spdlog::thread_pool(), spdlog::async_overflow_policy::overrun_oldest);
    spdlog::set_default_logger(logger);
    spdlog::set_level(spdlog::level::info);
}

void RouteToThread(ThreadCtx& ctx, const char* inst, uint32_t msgId) {
    auto& st = State();
    st.routes.push_back(std::make_unique<RouteCtx>(RouteCtx{&ctx, msgId}));
    if (detmw_subscribe(st.mw.get(), SESSION_TYPE_DTS, inst, msgId, OnRouteMsg,
                        st.routes.back().get()) != 0) {
        st.routes.pop_back();  // 订阅失败，不保留悬挂 RouteCtx
        spdlog::error("[StartUp] subscribe failed: {} {}", inst, msgId);
    }
}

// 按线程遍历生成 Sub 路由表注册订阅（替代手写 MSG_ID）
template <typename RouteArray, size_t N>
void RegisterSubRoutes(ThreadCtx& ctx, const RouteArray (&routes)[N]) {
    for (size_t i = 0; i < N; i++) {
        RouteToThread(ctx, routes[i].sessionInst, routes[i].msgId);
    }
}

}  // namespace

int StartUp(const char* cfg_path) {
    if (cfg_path == nullptr) {
        spdlog::error("[StartUp] cfg_path is null");
        return 1;
    }

    // 1. 日志（异步 spdlog）
    InitLog();

    // 2. 通信中间件（detmw，加载分进程生成配置 JSON）
    auto& st = State();
    st.mw.reset(detmw_init(cfg_path));
    if (!st.mw) {
        spdlog::error("[StartUp] detmw init failed");
        return 1;
    }
    DtsMwSet(st.mw.get());

    // 3. 声明业务域（detsched 代码层校验前提）+ 静态路由注册
    detsched::DeclareDomain(detsched::SchedPrio::Dts::DATA_PRIO);
    RegisterSubRoutes(st.task.ctx, kTaskSubRoutes);
    RegisterSubRoutes(st.data.ctx, kDataSubRoutes);
    RegisterSubRoutes(st.log.ctx, kLogSubRoutes);

    // 4. 创建业务线程（ThreadCtx + detsched 线程引擎，spec 表驱动）
    struct WorkerSpec {
        const char* name;
        int prio;
        EntryFn entry;
        Worker* out;
    };
    const WorkerSpec specs[] = {
        {"task", detsched::SchedPrio::Dts::TASK_PRIO, TaskEntry, &st.task},
        {"data", detsched::SchedPrio::Dts::DATA_PRIO, DataEntry, &st.data},
        {"log",  detsched::SchedPrio::Dts::LOG_PRIO,  LogEntry,  &st.log},
    };
    for (const auto& s : specs) {
        s.out->ctx.Init(s.name, s.entry);
        s.out->h = detsched::CreateThread(s.name, s.prio, ThreadEntry, &s.out->ctx);
    }

    spdlog::info("[StartUp] up (cfg={})", cfg_path);
    return 0;
}

void ShutDown() {
    auto& st = State();
    // 反序：先停业务线程，再销毁 detmw
    for (Worker* w : {&st.task, &st.data, &st.log}) {
        w->ctx.RequestStop();
        detsched::DestroyThread(w->h);
    }

    DtsMwSet(nullptr);
    st.mw.reset();
    st.routes.clear();
}

}  // namespace dts
