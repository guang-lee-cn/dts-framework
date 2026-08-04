#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace dts {

constexpr uint32_t kPingPongSize = 32 * 1024;

// 无锁乒乓 buffer：SPA 写写面，bb-agent 翻转读旧写面。单写者单读者，翻转原子切换。
class PingPongBuffer {
public:
    PingPongBuffer() = default;

    bool Write(const uint8_t* data, uint32_t len);
    uint32_t ReadAndFlip(uint8_t* out, uint32_t cap);

private:
    std::array<std::array<uint8_t, kPingPongSize>, 2> bufs_{};
    std::array<uint32_t, 2> off_{0, 0};
    std::atomic<uint32_t> cur_{0};
};

}  // namespace dts
