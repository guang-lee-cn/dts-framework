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
    // 维护节拍（100ms）：**绝对期限**——到点必投 MSG_ID_TIMER，消息多密都准点。
    // （相对期限 wait_until(now+100ms) 在持续负载下永不超时 → TTL 回收等维护任务停摆）
    auto nextTick = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    while (!ctx->m_stop.load()) {
        // 1) 维护节拍：到点强制投递（优先于消息处理，保 TTL/维护准点；
        //    投递 jitter ≤ 单条消息处理时间，由各线程预算兜底）
        if (std::chrono::steady_clock::now() >= nextTick) {
            if (ctx->m_entry != nullptr) {
                ctx->m_entry(ctx->m_status.load(), nullptr, MSG_ID_TIMER, nullptr, 0);
            }
            // 追赶：处理慢导致跨过多个节拍时跳过（维护任务幂等），不连投
            do {
                nextTick += std::chrono::milliseconds(100);
            } while (nextTick <= std::chrono::steady_clock::now());
        }

        // 2) 等消息（带绝对期限：到点由 1) 处理，不阻塞节拍）
        MailMsg msg;
        bool got = false;
        {
            std::unique_lock<std::mutex> lk(ctx->m_mailbox.m_mutex);
            ctx->m_mailbox.m_cv.wait_until(lk, nextTick, [&] {
                return !ctx->m_mailbox.EmptyLocked() || ctx->m_stop.load();
            });
            got = ctx->m_mailbox.TryPopLocked(msg);
        }
        if (!got) continue;

        // 3) 状态消息：更新线程状态（消息仍转发，状态动作 fn 挂在 msg_handler）
        if (msg.msgId == MSG_ID_STATUS && msg.payload && msg.payload->size() >= sizeof(StatusMsg)) {
            const auto* st = reinterpret_cast<const StatusMsg*>(msg.payload->data());
            ctx->m_status.store(st->status);
            dts::log::Info("[{}] state -> {}", ctx->m_name.c_str(), static_cast<int>(st->status));
        }

        // 4) 业务消息（sessionInst 由路由盖章，见 OnRouteMsg）
        if (ctx->m_entry != nullptr) {
            // 非 const：payload 所有权已移交给本线程，业务可原地处理（见 EntryFn 注释）
            void* p = (msg.payload && !msg.payload->empty()) ? msg.payload->data() : nullptr;
            const uint32_t n = msg.payload ? static_cast<uint32_t>(msg.payload->size()) : 0;
            ctx->m_entry(ctx->m_status.load(), msg.sessionInst, msg.msgId, p, n);
        }
    }
}

}  // namespace dts
