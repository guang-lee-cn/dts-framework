#pragma once

#include <cstdint>
#include <memory>

#include "detmw.h"

namespace detmw {

// 传输抽象：换 FastDDS/CycloneDDS/自研桶 只重写此接口实现（D10 唯一切换点）。
// 实现方持有一个不透明句柄，端点按 config 中声明的 role（sub/pub）创建。
struct TransportInterface {
    virtual ~TransportInterface() = default;

    // 建订阅端点（SEDP 匹配后，recv 回调常态触发）
    virtual int CreateReader(const endpoint& ep, recv_fn fn, void* ctx) = 0;

    // 建发布端点（预建，保证首包不丢）
    virtual int CreateWriter(const endpoint& ep) = 0;

    // 发布（进程外）：序列化 + 传输
    virtual int Send(const endpoint& ep, const uint8_t* data, uint32_t len) = 0;
};

// 传输工厂：实现方提供（FastDDS 等），Communicator 调用创建底层传输
std::unique_ptr<TransportInterface> CreateFastDdsTransport(int domain_id,
                                                           const char* process_name,
                                                           const char* static_xml_path);

}  // namespace detmw
