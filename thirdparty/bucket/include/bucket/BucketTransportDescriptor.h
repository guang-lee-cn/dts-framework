// 自研桶传输描述符：配置共享内存段大小 + 最大消息尺寸。
// 通过 user_transports 注册（配合 use_builtin_transports=false），
// create_transport() 工厂创建 BucketTransport，完全替代 FastDDS 内置 SHM。
#pragma once

#include <cstdint>

#include <fastdds/rtps/transport/TransportDescriptorInterface.hpp>

namespace bucket {

// 桶传输自定义 locator kind（避开 FastDDS 保留值：0,1,2,4,8,16+MAJOR,0x02000000）
constexpr int32_t kBucketLocatorKind = 32;

class BucketTransport;

class BucketTransportDescriptor : public eprosima::fastdds::rtps::TransportDescriptorInterface
{
public:

    static constexpr uint32_t default_segment_size = 64 * 1024 * 1024;    // 64MB：32K 突发 + 1MB 单条余量充足
    static constexpr uint32_t default_max_message_size = 8 * 1024 * 1024;  // 8MB：M 级消息传输侧上限

    BucketTransportDescriptor()
        : TransportDescriptorInterface(default_max_message_size, 10)
        , segment_size_(default_segment_size)
    {
    }

    eprosima::fastdds::rtps::TransportInterface* create_transport() const override;

    uint32_t min_send_buffer_size() const override
    {
        return maxMessageSize;
    }

    uint32_t segment_size() const
    {
        return segment_size_;
    }

    void segment_size(uint32_t size)
    {
        segment_size_ = size;
    }

    void max_message_size(uint32_t size)
    {
        maxMessageSize = size;
    }

private:

    uint32_t segment_size_;
};

}  // namespace bucket
