#include "log.h"
#include "dts_thread.h"

#include <chrono>
#include <cstdio>

namespace dts {

void ThreadCtx::Init(const std::string& name, EntryFn entry) {
    m_name = name;
    m_entry = entry;
}

void ThreadCtx::RequestStop() {
    m_stop.store(true);
    m_mailbox.m_cv.notify_all();
}

void ThreadEntry(void* arg) {
    auto* ctx = static_cast<ThreadCtx*>(arg);
    ThreadRun(ctx);
}

void ThreadRun(ThreadCtx* ctx) {
    dts::log::Info("[{}] thread up, state=IDLE, waiting for msg", ctx->m_name.c_str());
    while (!ctx->m_stop.load()) {
        MailMsg msg;
        bool got = false;
        bool timeout = false;
        {
            std::unique_lock<std::mutex> lk(ctx->m_mailbox.m_mutex);
            timeout = !ctx->m_mailbox.m_cv.wait_until(
                lk, std::chrono::steady_clock::now() + std::chrono::milliseconds(100),
                [&] { return !ctx->m_mailbox.EmptyLocked() || ctx->m_stop.load(); });
            got = ctx->m_mailbox.TryPopLocked(msg);
        }
        // 超时（无消息）：投递定时 tick，驱动业务线程 TimerWheel（不阻塞，固定 100ms 节拍）
        if (!got && timeout && ctx->m_entry != nullptr) {
            ctx->m_entry(ctx->m_status.load(), MSG_ID_TIMER, nullptr, 0);
            continue;
        }
        if (!got) continue;

        // 状态消息：更新线程状态（消息仍转发，状态动作 fn 挂在 msg_handler）
        if (msg.msgId == MSG_ID_STATUS && msg.payload.size() >= sizeof(StatusMsg)) {
            const auto* st = reinterpret_cast<const StatusMsg*>(msg.payload.data());
            ctx->m_status.store(st->status);
            dts::log::Info("[{}] state -> {}", ctx->m_name.c_str(), static_cast<int>(st->status));
        }

        if (ctx->m_entry != nullptr) {
            ctx->m_entry(ctx->m_status.load(), msg.msgId,
                         msg.payload.empty() ? nullptr : msg.payload.data(),
                         static_cast<uint32_t>(msg.payload.size()));
        }
    }
}

}  // namespace dts
