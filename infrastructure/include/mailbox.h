#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace dts {

// 线程邮箱消息：运行时只带 msgId（session 由静态路由绑定，不入线程内）
struct MailMsg {
    uint32_t msgId;
    std::vector<uint8_t> payload;
};

class Mailbox {
public:
    void Send(uint32_t msgId, const uint8_t* data, uint32_t len);
    bool EmptyLocked() const;                    // 调用者须持有 m_mutex
    bool TryPopLocked(MailMsg& out);             // 调用者须持有 m_mutex

    std::mutex m_mutex;
    std::condition_variable m_cv;

private:
    std::deque<MailMsg> m_queue;
};

inline void Mailbox::Send(uint32_t msgId, const uint8_t* data, uint32_t len) {
    MailMsg msg;
    msg.msgId = msgId;
    if (data != nullptr && len > 0) {
        msg.payload.assign(data, data + len);
    }
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_queue.push_back(std::move(msg));
    }
    m_cv.notify_one();
}

inline bool Mailbox::EmptyLocked() const { return m_queue.empty(); }

inline bool Mailbox::TryPopLocked(MailMsg& out) {
    if (m_queue.empty()) return false;
    out = std::move(m_queue.front());
    m_queue.pop_front();
    return true;
}

}  // namespace dts
