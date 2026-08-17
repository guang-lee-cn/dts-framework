#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace dts {

// 线程邮箱消息：业务组 + msgId + payload（unique_ptr 拥有，移所有权零拷贝）
struct MailMsg {
    uint32_t msgId = 0;
    // 业务组（第二层路由，sessionInst）。**借指针**：指向订阅端点（Subscription）的
    // session_inst 串，订阅生命周期 ≥ 任何在途消息，零拷贝；nullptr = 线程本地消息
    // （如 MSG_ID_TIMER，不属任何业务组）
    const char* sessionInst = nullptr;
    std::unique_ptr<std::vector<uint8_t>> payload;  // null = 无 payload
};

class Mailbox {
public:
    // 移所有权（零拷贝，大帧用）：reader 回调移交的 vec 直接入队，不拷贝
    void Send(uint32_t msgId, std::unique_ptr<std::vector<uint8_t>> payload,
              const char* sessionInst = nullptr);
    // 拷贝（小消息用，如 STATUS）
    void Send(uint32_t msgId, const uint8_t* data, uint32_t len,
              const char* sessionInst = nullptr);

    bool EmptyLocked() const;                    // 调用者须持有 m_mutex
    bool TryPopLocked(MailMsg& out);             // 调用者须持有 m_mutex

    std::mutex m_mutex;
    std::condition_variable m_cv;

private:
    std::deque<MailMsg> m_queue;
};

inline void Mailbox::Send(uint32_t msgId, std::unique_ptr<std::vector<uint8_t>> payload,
                          const char* sessionInst) {
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        MailMsg m;
        m.msgId = msgId;
        m.sessionInst = sessionInst;
        m.payload = std::move(payload);
        m_queue.push_back(std::move(m));
    }
    m_cv.notify_one();
}

inline void Mailbox::Send(uint32_t msgId, const uint8_t* data, uint32_t len,
                          const char* sessionInst) {
    auto payload = std::make_unique<std::vector<uint8_t>>();
    if (data != nullptr && len > 0) {
        payload->assign(data, data + len);
    }
    Send(msgId, std::move(payload), sessionInst);
}

inline bool Mailbox::EmptyLocked() const { return m_queue.empty(); }

inline bool Mailbox::TryPopLocked(MailMsg& out) {
    if (m_queue.empty()) return false;
    out = std::move(m_queue.front());
    m_queue.pop_front();
    return true;
}

}  // namespace dts
