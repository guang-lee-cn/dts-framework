#pragma once

// 定长 plain 字节类型（detmw 内部，单测可见）：
//   · 固定大小（配置 plain_size），is_plain/is_bounded=true → 无 CDR 封装（序列化纯 memcpy）
//   · plain + bounded → FastDDS DataSharing 自动启用（默认 AUTO，需 SHM 传输）：
//     发送侧 loan_sample 共享内存零拷贝；接收侧 take 拷贝出池（仍省 CDR/分配开销）
//   · 语义约束：本通道消息**严格定长**（Send 侧校验 len==plainSize，不匹配拒绝）

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <fastdds/dds/topic/TopicDataType.hpp>

namespace detmw {

class FixedBytesType : public eprosima::fastdds::dds::TopicDataType {
public:
    explicit FixedBytesType(uint32_t size)
        : m_size(size) {
        set_name("detmw::FixedBytes_" + std::to_string(size));
        max_serialized_type_size = size;
    }

    uint32_t Size() const { return m_size; }

    bool serialize(const void* const_data, eprosima::fastdds::rtps::SerializedPayload_t& payload,
                   eprosima::fastdds::dds::DataRepresentationId_t) override {
        const auto* vec = static_cast<const std::vector<uint8_t>*>(const_data);
        payload.reserve(m_size);
        std::memcpy(payload.data, vec->data(), m_size);
        payload.length = m_size;
        return true;
    }

    bool deserialize(eprosima::fastdds::rtps::SerializedPayload_t& payload, void* data) override {
        auto* vec = static_cast<std::vector<uint8_t>*>(data);
        const uint32_t n = payload.length < m_size ? payload.length : m_size;
        std::memcpy(vec->data(), payload.data, n);  // 定长 buffer 预分配（create_data）
        vec->resize(n);
        return true;
    }

    uint32_t calculate_serialized_size(const void*, eprosima::fastdds::dds::DataRepresentationId_t)
        override {
        return m_size;
    }

    void* create_data() override { return new std::vector<uint8_t>(m_size); }
    void delete_data(void* data) override { delete static_cast<std::vector<uint8_t>*>(data); }

    bool compute_key(eprosima::fastdds::rtps::SerializedPayload_t&,
                     eprosima::fastdds::rtps::InstanceHandle_t&, bool) override {
        return false;
    }

    bool compute_key(const void*, eprosima::fastdds::rtps::InstanceHandle_t&, bool) override {
        return false;
    }

    bool is_bounded() const override { return true; }
    bool is_plain(eprosima::fastdds::dds::DataRepresentationId_t) const override { return true; }

private:
    uint32_t m_size = 0;
};

}  // namespace detmw
