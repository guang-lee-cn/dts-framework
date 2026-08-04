// 自研桶传输实现：TransportInterface 对接层 + BucketChannelResource 收线程 + BucketSenderResource。
#include "bucket/BucketTransport.h"

#include <atomic>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <thread>

#include <fastdds/rtps/common/LocatorSelector.hpp>
#include <fastdds/rtps/common/LocatorsIterator.hpp>
#include <fastdds/rtps/common/PortParameters.hpp>
#include <fastdds/rtps/transport/NetworkBuffer.hpp>
#include <fastdds/rtps/transport/SenderResource.hpp>

namespace bucket {

using namespace eprosima::fastdds::rtps;

namespace {

std::string bucket_name(uint32_t port)
{
    return "/bucket_" + std::to_string(port);
}

Locator make_locator(uint32_t port, char type)
{
    Locator loc(kBucketLocatorKind, port);
    loc.address[0] = static_cast<octet>(type);
    return loc;
}

// 输出资源：send lambda 绑定到 transport.send（无零拷贝，直接拷入桶）
class BucketSenderResource : public SenderResource
{
public:

    explicit BucketSenderResource(BucketTransport& transport)
        : SenderResource(transport.kind())
    {
        clean_up = []()
                {
                };

        send_lambda_ = [&transport](
            const std::vector<NetworkBuffer>& buffers,
            uint32_t total_bytes,
            LocatorsIterator* destination_locators_begin,
            LocatorsIterator* destination_locators_end,
            const std::chrono::steady_clock::time_point& max_blocking_time_point,
            int32_t transport_priority) -> bool
                {
                    return transport.send(buffers, total_bytes, destination_locators_begin,
                                    destination_locators_end, max_blocking_time_point, transport_priority);
                };
    }
};

}  // namespace

// 输入通道：创建/接管读者桶 + 收线程（阻塞 Read → OnDataReceived）。
// 需在 bucket:: 作用域（头文件前向声明 BucketChannelResource，此处必须同名完整定义）。
class BucketChannelResource
{
public:

    BucketChannelResource(
            BucketTransport& transport,
            Locator locator,
            uint32_t max_msg_size,
            TransportReceiverInterface* receiver)
        : transport_(transport)
        , locator_(std::move(locator))
        , max_msg_size_(max_msg_size)
        , receiver_(receiver)
        , name_(bucket_name(locator_.port))
    {
        segment_ = std::shared_ptr<BucketSegment>(
            BucketSegment::Create(name_.c_str(), transport.configuration().segment_size()));
        if (!segment_)
        {
            segment_ = std::shared_ptr<BucketSegment>(BucketSegment::Open(name_.c_str()));
            if (segment_ && !segment_->ClaimOwnership())
            {
                segment_.reset();  // 被其他存活 reader 持有，不应并发
            }
        }
        if (!segment_)
        {
            throw std::runtime_error("bucket channel create/open failed: " + name_);
        }

        thread_ = std::thread(&BucketChannelResource::run, this);
    }

    ~BucketChannelResource()
    {
        stop();
    }

    void stop()
    {
        alive_.store(false);
        if (thread_.joinable())
        {
            thread_.join();  // 收线程 200ms 内退出
        }
        BucketSegment::Remove(name_.c_str());
        segment_.reset();
    }

    const Locator& locator() const
    {
        return locator_;
    }

private:

    void run()
    {
        std::vector<uint8_t> buf(max_msg_size_);
        while (alive_.load())
        {
            uint32_t len = 0;
            auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
            if (!segment_->Read(buf.data(), max_msg_size_, len, deadline))
            {
                continue;  // 空/超时：轮询 alive
            }
            if (len > max_msg_size_)
            {
                continue;  // 防御（条目已消费）
            }
            // 桶不携带发送方信息：remote 置为输入 locator（RTPS 路由以 GUID 为准）
            receiver_->OnDataReceived(buf.data(), len, locator_, locator_);
        }
    }

    BucketTransport& transport_;
    Locator locator_;
    uint32_t max_msg_size_;
    TransportReceiverInterface* receiver_;
    std::string name_;
    std::shared_ptr<BucketSegment> segment_;
    std::thread thread_;
    std::atomic<bool> alive_{true};
};

// ---- 生命周期 ----

BucketTransport::BucketTransport(const BucketTransportDescriptor& descriptor)
    : TransportInterface(kBucketLocatorKind)
    , configuration_(descriptor)
{
}

BucketTransport::~BucketTransport()
{
    for (auto* channel : input_channels_)
    {
        delete channel;
    }
    input_channels_.clear();
}

bool BucketTransport::init(
        const PropertyPolicy*,
        const uint32_t&)
{
    if (configuration_.segment_size() < configuration_.maxMessageSize)
    {
        return false;
    }
    return true;
}

// ---- locator 处理 ----

bool BucketTransport::IsLocatorSupported(const Locator& locator) const
{
    return locator.kind == transport_kind_;
}

bool BucketTransport::IsInputChannelOpen(const Locator& locator) const
{
    std::lock_guard<std::mutex> lock(input_channels_mutex_);
    for (auto* channel : input_channels_)
    {
        if (channel->locator() == locator)
        {
            return true;
        }
    }
    return false;
}

bool BucketTransport::is_locator_allowed(const Locator& locator) const
{
    return IsLocatorSupported(locator);
}

bool BucketTransport::is_locator_reachable(const Locator_t& locator)
{
    if (!IsLocatorSupported(locator))
    {
        return false;
    }
    return open_bucket(locator.port) != nullptr;  // 读者已建桶即可达
}

Locator BucketTransport::RemoteToMainLocal(const Locator& remote) const
{
    if (!IsLocatorSupported(remote))
    {
        return Locator();
    }
    Locator main_local(remote);
    main_local.set_Invalid_Address();
    return main_local;
}

bool BucketTransport::transform_remote_locator(
        const Locator& remote_locator,
        Locator& result_locator,
        bool,
        bool) const
{
    if (IsLocatorSupported(remote_locator))
    {
        result_locator = remote_locator;
        return true;
    }
    return false;
}

LocatorList BucketTransport::NormalizeLocator(const Locator& locator)
{
    LocatorList list;
    list.push_back(locator);
    return list;
}

bool BucketTransport::is_local_locator(const Locator& locator) const
{
    (void)locator;
    return true;  // 桶只在本机
}

bool BucketTransport::is_localhost_allowed() const
{
    return true;
}

TransportDescriptorInterface* BucketTransport::get_configuration()
{
    return &configuration_;
}

// ---- 输出通道 ----

bool BucketTransport::OpenOutputChannel(
        SendResourceList& sender_resource_list,
        const Locator& locator)
{
    if (!IsLocatorSupported(locator))
    {
        return false;
    }
    for (auto& sender_resource : sender_resource_list)
    {
        if (sender_resource->kind() == transport_kind_)
        {
            return true;  // 复用已有
        }
    }
    sender_resource_list.emplace_back(new BucketSenderResource(*this));
    return true;
}

bool BucketTransport::OpenOutputChannels(
        SendResourceList& send_resource_list,
        const LocatorSelectorEntry& locator_selector_entry)
{
    bool success = false;
    for (size_t i = 0; i < locator_selector_entry.state.unicast.size(); ++i)
    {
        size_t index = locator_selector_entry.state.unicast[i];
        success |= OpenOutputChannel(send_resource_list, locator_selector_entry.unicast[index]);
    }
    return success;
}

void BucketTransport::CloseOutputChannels(
        SendResourceList& sender_resource_list,
        const LocatorSelectorEntry& locator_selector_entry)
{
    (void)sender_resource_list;
    (void)locator_selector_entry;
}

// ---- 输入通道 ----

bool BucketTransport::OpenInputChannel(
        const Locator& locator,
        TransportReceiverInterface* receiver,
        uint32_t max_msg_size)
{
    std::lock_guard<std::mutex> lock(input_channels_mutex_);

    if (!IsLocatorSupported(locator))
    {
        return false;
    }
    for (auto* channel : input_channels_)
    {
        if (channel->locator() == locator)
        {
            return true;  // 同 locator 复用
        }
    }

    try
    {
        input_channels_.push_back(create_channel(locator, max_msg_size, receiver));
    }
    catch (const std::exception&)
    {
        return false;
    }
    return true;
}

bool BucketTransport::CloseInputChannel(const Locator& locator)
{
    std::lock_guard<std::mutex> lock(input_channels_mutex_);

    for (auto it = input_channels_.begin(); it != input_channels_.end(); ++it)
    {
        if ((*it)->locator() == locator)
        {
            delete *it;  // stop 收线程 + Remove 桶
            input_channels_.erase(it);
            return true;
        }
    }
    return false;
}

bool BucketTransport::DoInputLocatorsMatch(
        const Locator& left,
        const Locator& right) const
{
    return left.kind == right.kind && left.port == right.port;
}

// ---- 默认 locator 生成 ----

void BucketTransport::AddDefaultOutputLocator(LocatorList& default_list)
{
    (void)default_list;
}

bool BucketTransport::getDefaultMetatrafficMulticastLocators(
        LocatorList& locators,
        uint32_t metatraffic_multicast_port) const
{
    locators.push_back(make_locator(metatraffic_multicast_port, 'M'));
    return true;
}

bool BucketTransport::getDefaultMetatrafficUnicastLocators(
        LocatorList& locators,
        uint32_t metatraffic_unicast_port) const
{
    locators.push_back(make_locator(metatraffic_unicast_port, 'U'));
    return true;
}

bool BucketTransport::getDefaultUnicastLocators(
        LocatorList& locators,
        uint32_t unicast_port) const
{
    Locator locator = make_locator(unicast_port, 'U');
    fillUnicastLocator(locator, unicast_port);
    locators.push_back(locator);
    return true;
}

bool BucketTransport::fillMetatrafficMulticastLocator(
        Locator& locator,
        uint32_t metatraffic_multicast_port) const
{
    if (locator.port == 0)
    {
        locator.port = metatraffic_multicast_port;
    }
    return true;
}

bool BucketTransport::fillMetatrafficUnicastLocator(
        Locator& locator,
        uint32_t metatraffic_unicast_port) const
{
    if (locator.port == 0)
    {
        locator.port = metatraffic_unicast_port;
    }
    return true;
}

bool BucketTransport::configureInitialPeerLocator(
        Locator& locator,
        const PortParameters& port_params,
        uint32_t domainId,
        LocatorList& list) const
{
    if (locator.port == 0)
    {
        for (uint32_t i = 0; i < configuration_.max_initial_peers_range(); ++i)
        {
            Locator aux_locator(locator);
            aux_locator.port = port_params.getUnicastPort(domainId, i);
            list.push_back(aux_locator);
        }
    }
    else
    {
        list.push_back(locator);
    }
    return true;
}

bool BucketTransport::fillUnicastLocator(
        Locator& locator,
        uint32_t well_known_port) const
{
    if (locator.port == 0)
    {
        locator.port = well_known_port;
    }
    return true;
}

// ---- 选择 + 发送 ----

void BucketTransport::select_locators(LocatorSelector& selector) const
{
    auto& entries = selector.transport_starts();

    for (size_t i = 0; i < entries.size(); ++i)
    {
        LocatorSelectorEntry* entry = entries[i];
        if (entry->transport_should_process)
        {
            bool selected = false;
            // 桶无组播优势（同机点对点），只选单播 locator
            for (size_t j = 0; j < entry->unicast.size(); ++j)
            {
                if (IsLocatorSupported(entry->unicast[j]) && !selector.is_selected(entry->unicast[j]))
                {
                    entry->state.unicast.push_back(j);
                    selected = true;
                }
            }
            if (selected)
            {
                selector.select(i);
            }
        }
    }
}

bool BucketTransport::send(
        const std::vector<NetworkBuffer>& buffers,
        uint32_t total_bytes,
        LocatorsIterator* destination_locators_begin,
        LocatorsIterator* destination_locators_end,
        const std::chrono::steady_clock::time_point& max_blocking_time_point,
        const int32_t)
{
    std::vector<uint8_t> payload(total_bytes);
    uint32_t pos = 0;
    for (const auto& buffer : buffers)
    {
        std::memcpy(payload.data() + pos, buffer.buffer, buffer.size);
        pos += buffer.size;
    }

    bool ret = true;
    LocatorsIterator& it = *destination_locators_begin;
    while (it != *destination_locators_end)
    {
        if (IsLocatorSupported(*it))
        {
            auto segment = open_bucket((*it).port);
            if (!segment || !segment->Write(payload.data(), total_bytes, max_blocking_time_point))
            {
                ret = false;
            }
        }
        ++it;
    }
    return ret;
}

// ---- 内部 ----

std::shared_ptr<BucketSegment> BucketTransport::open_bucket(uint32_t port)
{
    std::lock_guard<std::mutex> lock(buckets_mutex_);
    auto it = buckets_.find(port);
    if (it != buckets_.end())
    {
        return it->second;
    }
    auto segment = std::shared_ptr<BucketSegment>(BucketSegment::Open(bucket_name(port).c_str()));
    if (!segment)
    {
        return nullptr;  // 读者尚未创建该桶
    }
    buckets_[port] = segment;
    return segment;
}

BucketChannelResource* BucketTransport::create_channel(
        const Locator& locator,
        uint32_t max_msg_size,
        TransportReceiverInterface* receiver)
{
    return new BucketChannelResource(*this, locator, max_msg_size, receiver);
}

}  // namespace bucket
