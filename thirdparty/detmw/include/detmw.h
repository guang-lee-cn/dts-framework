#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace detmw {

// 统一接收回调：消息到达时触发（SEDP 匹配完成后常态入口）。
// data 移交所有权（unique_ptr，零拷贝：reader 回调不拷贝/不释放，回调消费后自动释放）
using recv_fn = void (*)(void* user_ctx, std::unique_ptr<std::vector<uint8_t>> data);

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

    // 发（进程内）：目标为本进程某线程。命中本进程订阅者 → mailbox 直投（免 DDS
    // 序列化，D7/D8）；未命中 → 回退 transport（对端可能在别的进程）。
    // 接收侧不变：仍经订阅回调 OnRouteMsg 统一路由进目标线程 mailbox（契约 detmw.md §3）
    int publish_internal(const endpoint& dst, const uint8_t* data, uint32_t len);

    // 调试：dump 配置端点
    int dump(char* buf, size_t cap) const;

    // 装配状态：构造成功（配置解析 + participant 起立）返回 true。
    // false = cfg 缺失/解析失败/transport 起不来，订阅与发布均不可用
    bool good() const;

private:
    struct Impl;
    Impl* m_impl;
};

}  // namespace detmw
