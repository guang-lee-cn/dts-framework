#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>

namespace detmw {

// 统一接收回调：消息到达时触发（SEDP 匹配完成后常态入口）
using recv_fn = void (*)(void* user_ctx, const uint8_t* data, uint32_t len);

// 统一寻址键：发布方与订阅方共享同一 endpoint，键即约定
struct endpoint {
    std::string session_type;
    std::string session_inst;
    uint32_t msg_id;

    bool operator==(const endpoint& o) const {
        return msg_id == o.msg_id && session_type == o.session_type &&
               session_inst == o.session_inst;
    }
    std::string ToString() const {
        return session_type + "." + session_inst + "." + std::to_string(msg_id);
    }
};

struct endpoint_hash {
    size_t operator()(const endpoint& e) const {
        size_t h1 = std::hash<std::string>{}(e.session_type);
        size_t h2 = std::hash<std::string>{}(e.session_inst);
        return h1 ^ (h2 << 1) ^ (static_cast<size_t>(e.msg_id) << 3);
    }
};

// 通信站点：每进程一个，承载订阅/发布端点。构造=init，析构=destroy。
// 底层传输经 TransportInterface 隔离，可替换。
class Communicator {
public:
    explicit Communicator(const char* cfg_path);
    ~Communicator();

    Communicator(const Communicator&) = delete;
    Communicator& operator=(const Communicator&) = delete;

    // 收：按 endpoint 建 reader + 注册常态回调（SEDP 内部完成）
    int subscribe(const endpoint& src, recv_fn fn, void* ctx);

    // 发（进程外）：走 DDS，序列化 + 传输
    int publish_external(const endpoint& dst, const uint8_t* data, uint32_t len);

    // 发（进程内）：目标为本进程某线程，mailbox 直通免序列化（取 dst.msg_id 投递）
    int publish_internal(const endpoint& dst, const uint8_t* data, uint32_t len);

    // 调试：dump 配置端点
    int dump(char* buf, size_t cap) const;

private:
    struct Impl;
    Impl* m_impl;
};

}  // namespace detmw
