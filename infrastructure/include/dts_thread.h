#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "dts_def.h"
#include "mailbox.h"

namespace dts {

// 线程入口：外部消息 -> 线程业务（对齐统一回调接口）
using EntryFn = void (*)(ThreadStatus status, uint32_t msgId, const uint8_t* msg, uint32_t len);

// 线程业务上下文：消息处理（mailbox/状态机/entry 分发）。不依赖 detsched。
struct ThreadCtx {
    std::string m_name;
    std::atomic<ThreadStatus> m_status{ThreadStatus::IDLE};
    std::atomic<bool> m_stop{false};
    Mailbox m_mailbox;
    EntryFn m_entry = nullptr;

    void Init(const std::string& name, EntryFn entry);
    void RequestStop();   // 仅置业务循环退出标志，detsched 回收由组合根负责
};

// detsched 线程 fn 适配桥：detsched 调本函数 -> ThreadRun 业务循环
void ThreadEntry(void* arg);
void ThreadRun(ThreadCtx* ctx);

}  // namespace dts
