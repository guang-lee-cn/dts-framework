#pragma once

#include <cstdint>

namespace dts {

// 模拟网管 kafka：订阅上报通道，打印收到的上报块
class KafkaMock {
public:
    void Start();

    static uint64_t ReceivedCount() { return m_received; }
    static uint64_t ReceivedBytes() { return m_bytes; }
    static void Record(uint64_t len) {
        ++m_received;
        m_bytes += len;
    }

private:
    static inline uint64_t m_received = 0;
    static inline uint64_t m_bytes = 0;
};

}  // namespace dts
