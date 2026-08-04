#include "dts_agent/pingpong.h"

#include <algorithm>
#include <cstring>

namespace dts {

bool PingPongBuffer::Write(const uint8_t* data, uint32_t len) {
    uint32_t idx = cur_.load(std::memory_order_relaxed);
    uint32_t& off = off_[idx];
    if (len == 0 || off + len > kPingPongSize) {
        return false;  // 写满丢弃（SPA 自控）
    }
    std::memcpy(bufs_[idx].data() + off, data, len);
    off += len;
    return true;
}

uint32_t PingPongBuffer::ReadAndFlip(uint8_t* out, uint32_t cap) {
    uint32_t old = cur_.fetch_xor(1, std::memory_order_relaxed);  // 翻转，返回旧写面
    uint32_t n = std::min(off_[old], cap);
    if (n > 0) {
        std::memcpy(out, bufs_[old].data(), n);
        off_[old] = 0;  // 旧面交还写者复用
    }
    return n;
}

}  // namespace dts
