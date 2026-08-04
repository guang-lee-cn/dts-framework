// 端到端 RTPS 验证：自研桶传输替代 FastDDS 内置 SHM，跑完整 FastDDS 收发。
//   bucket_rtps_test sub <count> <size> [timeout_s]
//   bucket_rtps_test pub <count> <size> [delay_us] [wait_s]
// 载荷前 4 字节 = seq(LE u32)，sub 检测空洞。与 fastdds_burst_repro 同协议，可直接对比丢包。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include <fastdds/dds/core/ReturnCode.hpp>
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/DataReaderListener.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/topic/TopicDataType.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>

#include "bucket/BucketTransportDescriptor.h"

using namespace eprosima::fastdds::dds;

namespace {

class BytesType : public TopicDataType
{
public:

    BytesType()
    {
        set_name("Bytes");
        max_serialized_type_size = 4 * 1024 * 1024;
    }

    bool serialize(const void* d, eprosima::fastdds::rtps::SerializedPayload_t& p,
                   DataRepresentationId_t) override
    {
        const auto* v = static_cast<const std::vector<uint8_t>*>(d);
        p.reserve(static_cast<uint32_t>(v->size()));
        std::memcpy(p.data, v->data(), v->size());
        p.length = static_cast<uint32_t>(v->size());
        return true;
    }

    bool deserialize(eprosima::fastdds::rtps::SerializedPayload_t& p, void* d) override
    {
        auto* v = static_cast<std::vector<uint8_t>*>(d);
        v->assign(p.data, p.data + p.length);
        return true;
    }

    uint32_t calculate_serialized_size(const void* d, DataRepresentationId_t) override
    {
        return static_cast<uint32_t>(static_cast<const std::vector<uint8_t>*>(d)->size());
    }

    void* create_data() override { return new std::vector<uint8_t>(); }
    void delete_data(void* d) override { delete static_cast<std::vector<uint8_t>*>(d); }

    bool compute_key(eprosima::fastdds::rtps::SerializedPayload_t&,
                     eprosima::fastdds::rtps::InstanceHandle_t&, bool) override
    {
        return false;
    }

    bool compute_key(const void*, eprosima::fastdds::rtps::InstanceHandle_t&, bool) override
    {
        return false;
    }

    bool is_bounded() const override { return false; }
    bool is_plain(DataRepresentationId_t) const override { return false; }
};

class Listener : public DataReaderListener
{
public:

    std::atomic<uint64_t> recv{0};
    std::atomic<bool> gap{false};
    std::atomic<uint32_t> last_seq{0};

    void on_data_available(DataReader* reader) override
    {
        BytesType type;
        while (true)
        {
            SampleInfo info;
            void* data = type.create_data();
            if (reader->take_next_sample(data, &info) != RETCODE_OK)
            {
                type.delete_data(data);
                break;
            }
            if (info.valid_data)
            {
                auto* v = static_cast<std::vector<uint8_t>*>(data);
                uint32_t seq = 0;
                if (v->size() >= 4)
                {
                    std::memcpy(&seq, v->data(), 4);
                }
                recv.fetch_add(1);
                uint32_t prev = last_seq.exchange(seq);
                if (prev != 0 && seq != prev + 1)
                {
                    gap.store(true);
                }
            }
            type.delete_data(data);
        }
    }
};

DomainParticipant* create_participant(const char* name)
{
    auto* factory = DomainParticipantFactory::get_instance();
    DomainParticipantQos pqos = PARTICIPANT_QOS_DEFAULT;
    pqos.name(name);
    pqos.transport().use_builtin_transports = false;

    auto bucket = std::make_shared<bucket::BucketTransportDescriptor>();
    bucket->segment_size(64 * 1024 * 1024);
    bucket->max_message_size(8 * 1024 * 1024);
    pqos.transport().user_transports.push_back(bucket);

    return factory->create_participant(0, pqos);
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 4)
    {
        std::printf("usage: %s <pub|sub> <count> <size> [extra]\n", argv[0]);
        return 2;
    }
    std::string mode = argv[1];
    uint32_t count = std::strtoul(argv[2], nullptr, 10);
    uint32_t size = std::strtoul(argv[3], nullptr, 10);

    DomainParticipant* participant = create_participant(mode.c_str());
    if (!participant)
    {
        std::printf("[rtps] participant create failed\n");
        return 1;
    }
    // 类型对象须存活至 participant 析构之后：堆分配并泄漏（与 fastdds_burst_repro 一致）
    auto* type = new BytesType();
    TypeSupport ts(type);
    ts.register_type(participant);
    Topic* topic = participant->create_topic("burst", type->get_name(), TOPIC_QOS_DEFAULT);

    if (mode == "sub")
    {
        uint32_t timeout_s = (argc > 4) ? std::strtoul(argv[4], nullptr, 10) : 20;
        Listener listener;
        Subscriber* subscriber = participant->create_subscriber(SUBSCRIBER_QOS_DEFAULT);
        DataReaderQos rqos = DATAREADER_QOS_DEFAULT;
        rqos.reliability().kind = RELIABLE_RELIABILITY_QOS;
        rqos.history().kind = KEEP_LAST_HISTORY_QOS;
        rqos.history().depth = 100;
        if (!subscriber->create_datareader(topic, rqos, &listener))
        {
            std::printf("[rtps] reader create failed\n");
            return 1;
        }
        std::printf("[rtps] sub waiting (expect %u x %uB over bucket transport)...\n", count, size);
        for (int i = 0; i < static_cast<int>(timeout_s * 10) && listener.recv.load() < count; i++)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
        uint64_t recv = listener.recv.load();
        std::printf("[rtps] sub recv=%llu/%u gap=%s -> %s\n",
                    static_cast<unsigned long long>(recv), count,
                    listener.gap.load() ? "YES" : "no",
                    (recv == count && !listener.gap.load()) ? "PASS (lossless)" : "LOSS/GAP");
        DomainParticipantFactory::get_instance()->delete_participant(participant);
        return (recv == count && !listener.gap.load()) ? 0 : 1;
    }

    uint32_t delay_us = (argc > 4) ? std::strtoul(argv[4], nullptr, 10) : 0;
    uint32_t wait_s = (argc > 5) ? std::strtoul(argv[5], nullptr, 10) : 3;
    Publisher* publisher = participant->create_publisher(PUBLISHER_QOS_DEFAULT);
    DataWriterQos wqos = DATAWRITER_QOS_DEFAULT;
    wqos.reliability().kind = RELIABLE_RELIABILITY_QOS;
    wqos.history().kind = KEEP_LAST_HISTORY_QOS;
    wqos.history().depth = 100;
    DataWriter* writer = publisher->create_datawriter(topic, wqos);
    if (!writer)
    {
        std::printf("[rtps] writer create failed\n");
        return 1;
    }
    std::printf("[rtps] pub waiting %us...\n", wait_s);
    std::this_thread::sleep_for(std::chrono::seconds(wait_s));

    std::vector<uint8_t> pkt(size, 0);
    uint32_t fail = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < count; i++)
    {
        std::memcpy(pkt.data(), &i, 4);
        if (writer->write(&pkt) != RETCODE_OK)
        {
            fail++;
        }
        if (delay_us > 0)
        {
            std::this_thread::sleep_for(std::chrono::microseconds(delay_us));
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("[rtps] pub wrote %u x %uB in %.1fms write_fail=%u\n", count, size, ms, fail);

    // 显式销毁，避免 static teardown 时序问题（这也是桶传输 vs FastDDS SHM 的验证点）
    DomainParticipantFactory::get_instance()->delete_participant(participant);
    return 0;
}
