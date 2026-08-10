#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace dts {

// 线程邮箱消息：msgId + payload（unique_ptr 拥有，移所有权零拷贝）
struct MailMsg {
    uint32_t msgId = 0;
    std::unique_ptr<std::vector<uint8_t>> payload;  // null = 无 payload
};

class Mailbox {
public:
    // 移所有权（零拷贝，大帧用）：reader 回调移交的 vec 直接入队，不拷贝
    void Send(uint32_t msgId, std::unique_ptr<std::vector<uint8_t>> payload);
    // 拷贝（小消息用，如 STATUS）
    void Send(uint32_t msgId, const uint8_t* data, uint32_t len);

    bool EmptyLocked() const;                    // 调用者须持有 m_mutex
    bool TryPopLocked(MailMsg& out);             // 调用者须持有 m_mutex

    std::mutex m_mutex;
    std::condition_variable m_cv;

private:
    std::deque<MailMsg> m_queue;
};

inline void Mailbox::Send(uint32_t msgId, std::unique_ptr<std::vector<uint8_t>> payload) {
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_queue.push_back(MailMsg{msgId, std::move(payload)});
    }
    m_cv.notify_one();
}

inline void Mailbox::Send(uint32_t msgId, const uint8_t* data, uint32_t len) {
    auto payload = std::make_unique<std::vector<uint8_t>>();
    if (data != nullptr && len > 0) {
        payload->assign(data, data + len);
    }
    Send(msgId, std::move(payload));
}

inline bool Mailbox::EmptyLocked() const { return m_queue.empty(); }

inline bool Mailbox::TryPopLocked(MailMsg& out) {
    if (m_queue.empty()) return false;
    out = std::move(m_queue.front());
    m_queue.pop_front();
    return true;
}

}  // namespace dts
