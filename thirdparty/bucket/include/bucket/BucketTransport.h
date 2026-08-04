// 自研桶传输：实现 FastDDS TransportInterface，替代内置 SharedMemTransport。
//
// 结构完全参考 FastDDS SharedMemTransport（API 面 + locator 编码一致），
// 传输核心换成 BucketSegment（共享内存环），从根上绕开 SHM 的 Port/WatchTask 堆损坏。
//
// locator 编码：kind=kBucketLocatorKind，port=RTPS 端口（域派生），address[0]='U'/'M'。
// 桶命名：/bucket_<port>（输入通道 = 读者桶，写者向其写入，一个读者一个桶）。
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <fastdds/rtps/common/Locator.hpp>
#include <fastdds/rtps/transport/TransportInterface.hpp>
#include <fastdds/rtps/transport/TransportReceiverInterface.hpp>

#include "bucket/BucketSegment.h"
#include "bucket/BucketTransportDescriptor.h"

namespace bucket {

using eprosima::fastdds::rtps::Locator;
using eprosima::fastdds::rtps::LocatorList;
using eprosima::fastdds::rtps::LocatorSelector;
using eprosima::fastdds::rtps::LocatorSelectorEntry;
using eprosima::fastdds::rtps::LocatorsIterator;
using eprosima::fastdds::rtps::NetworkBuffer;
using eprosima::fastdds::rtps::PortParameters;
using eprosima::fastdds::rtps::PropertyPolicy;
using eprosima::fastdds::rtps::SendResourceList;
using eprosima::fastdds::rtps::TransportDescriptorInterface;
using eprosima::fastdds::rtps::TransportInterface;
using eprosima::fastdds::rtps::TransportReceiverInterface;

class BucketChannelResource;

class BucketTransport : public TransportInterface
{
public:

    explicit BucketTransport(const BucketTransportDescriptor& descriptor);
    ~BucketTransport() override;

    bool init(
            const eprosima::fastdds::rtps::PropertyPolicy* properties = nullptr,
            const uint32_t& max_msg_size_no_frag = 0) override;

    // ---- locator 处理 ----
    bool IsInputChannelOpen(const Locator&) const override;
    bool IsLocatorSupported(const Locator&) const override;
    bool is_locator_allowed(const Locator&) const override;
    bool is_locator_reachable(const eprosima::fastdds::rtps::Locator_t& locator) override;
    Locator RemoteToMainLocal(const Locator&) const override;
    bool transform_remote_locator(
            const Locator& remote_locator,
            Locator& result_locator,
            bool allowed_remote_localhost,
            bool allowed_local_localhost) const override;
    eprosima::fastdds::rtps::LocatorList NormalizeLocator(const Locator&) override;
    bool is_local_locator(const Locator&) const override;
    bool is_localhost_allowed() const override;
    eprosima::fastdds::rtps::TransportDescriptorInterface* get_configuration() override;

    // ---- 输出通道（send 资源）----
    bool OpenOutputChannel(
            eprosima::fastdds::rtps::SendResourceList& sender_resource_list,
            const Locator&) override;
    bool OpenOutputChannels(
            eprosima::fastdds::rtps::SendResourceList& sender_resource_list,
            const eprosima::fastdds::rtps::LocatorSelectorEntry& locator_selector_entry) override;
    void CloseOutputChannels(
            eprosima::fastdds::rtps::SendResourceList& sender_resource_list,
            const eprosima::fastdds::rtps::LocatorSelectorEntry& locator_selector_entry) override;

    // ---- 输入通道 ----
    bool OpenInputChannel(
            const Locator&,
            eprosima::fastdds::rtps::TransportReceiverInterface*,
            uint32_t) override;
    bool CloseInputChannel(const Locator&) override;
    bool DoInputLocatorsMatch(const Locator&, const Locator&) const override;

    // ---- 默认 locator 生成 ----
    void AddDefaultOutputLocator(eprosima::fastdds::rtps::LocatorList&) override;
    bool getDefaultMetatrafficMulticastLocators(
            eprosima::fastdds::rtps::LocatorList& locators,
            uint32_t metatraffic_multicast_port) const override;
    bool getDefaultMetatrafficUnicastLocators(
            eprosima::fastdds::rtps::LocatorList& locators,
            uint32_t metatraffic_unicast_port) const override;
    bool getDefaultUnicastLocators(
            eprosima::fastdds::rtps::LocatorList& locators,
            uint32_t unicast_port) const override;
    bool fillMetatrafficMulticastLocator(Locator&, uint32_t) const override;
    bool fillMetatrafficUnicastLocator(Locator&, uint32_t) const override;
    bool configureInitialPeerLocator(
            Locator& locator,
            const eprosima::fastdds::rtps::PortParameters& port_params,
            uint32_t domainId,
            eprosima::fastdds::rtps::LocatorList& list) const override;
    bool fillUnicastLocator(Locator&, uint32_t) const override;

    uint32_t max_recv_buffer_size() const override
    {
        return (std::numeric_limits<uint32_t>::max)();
    }

    void select_locators(eprosima::fastdds::rtps::LocatorSelector& selector) const override;

    // ---- 发送（SendResource 的 lambda 绑定到此）----
    bool send(
            const std::vector<eprosima::fastdds::rtps::NetworkBuffer>& buffers,
            uint32_t total_bytes,
            eprosima::fastdds::rtps::LocatorsIterator* destination_locators_begin,
            eprosima::fastdds::rtps::LocatorsIterator* destination_locators_end,
            const std::chrono::steady_clock::time_point& max_blocking_time_point,
            const int32_t transport_priority);

    const BucketTransportDescriptor& configuration() const
    {
        return configuration_;
    }

private:

    using TransportInterface::transform_remote_locator;

    std::shared_ptr<BucketSegment> open_bucket(uint32_t port);   // 写侧打开（读者已创建）
    BucketChannelResource* create_channel(
            const Locator& locator,
            uint32_t max_msg_size,
            eprosima::fastdds::rtps::TransportReceiverInterface* receiver);

    BucketTransportDescriptor configuration_;
    mutable std::mutex input_channels_mutex_;
    std::vector<BucketChannelResource*> input_channels_;
    mutable std::mutex buckets_mutex_;
    std::map<uint32_t, std::shared_ptr<BucketSegment>> buckets_;  // 写侧打开缓存
};

}  // namespace bucket
