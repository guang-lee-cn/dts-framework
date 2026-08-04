// 最小 FastDDS 可靠突发复现：不经 detmw，直接 FastDDS
// 判定多进程可靠紧突发的丢包是 FastDDS 版本 bug 还是上层问题
// 用法：
//   fastdds_burst_repro sub <count> <size> [depth] [timeout_s]   // reader
//   fastdds_burst_repro pub <count> <size> [depth] [delay_us] [wait_s]  // writer
// 载荷前 4 字节 = seq(LE u32)，sub 检测空洞
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

using namespace eprosima::fastdds::dds;

namespace {

class BytesType : public TopicDataType {
public:
    BytesType() {
        set_name("Bytes");
        max_serialized_type_size = 4 * 1024 * 1024;
    }
    bool serialize(const void* d, eprosima::fastdds::rtps::SerializedPayload_t& p,
                   DataRepresentationId_t) override {
        const auto* v = static_cast<const std::vector<uint8_t>*>(d);
        p.reserve(static_cast<uint32_t>(v->size()));
        std::memcpy(p.data, v->data(), v->size());
        p.length = static_cast<uint32_t>(v->size());
        return true;
    }
    bool deserialize(eprosima::fastdds::rtps::SerializedPayload_t& p, void* d) override {
        auto* v = static_cast<std::vector<uint8_t>*>(d);
        v->assign(p.data, p.data + p.length);
        return true;
    }
    uint32_t calculate_serialized_size(const void* d, DataRepresentationId_t) override {
        return static_cast<uint32_t>(static_cast<const std::vector<uint8_t>*>(d)->size());
    }
    void* create_data() override { return new std::vector<uint8_t>(); }
    void delete_data(void* d) override { delete static_cast<std::vector<uint8_t>*>(d); }
    bool compute_key(eprosima::fastdds::rtps::SerializedPayload_t&,
                     eprosima::fastdds::rtps::InstanceHandle_t&, bool) override {
        return false;
    }
    bool compute_key(const void*, eprosima::fastdds::rtps::InstanceHandle_t&, bool) override {
        return false;
    }
    bool is_bounded() const override { return false; }
    bool is_plain(DataRepresentationId_t) const override { return false; }
};

class Listener : public DataReaderListener {
public:
    explicit Listener(BytesType& type) : m_type(type) {}
    std::atomic<uint64_t> recv{0};
    std::atomic<bool> gap{false};
    std::atomic<uint32_t> lastSeq{0};

    void on_data_available(DataReader* reader) override {
        while (true) {
            SampleInfo info;
            void* data = m_type.create_data();
            if (reader->take_next_sample(data, &info) != RETCODE_OK) {
                m_type.delete_data(data);
                break;
            }
            if (info.valid_data) {
                auto* v = static_cast<std::vector<uint8_t>*>(data);
                uint32_t seq = 0;
                if (v->size() >= 4) {
                    std::memcpy(&seq, v->data(), 4);
                }
                recv.fetch_add(1);
                uint32_t prev = lastSeq.exchange(seq);
                if (prev != 0 && seq != prev + 1) {
                    gap.store(true);
                }
            }
            m_type.delete_data(data);
        }
    }

private:
    BytesType& m_type;
};

// 建 participant + type + topic（供 pub/sub 共用）
struct Ctx {
    DomainParticipant* participant = nullptr;
    Publisher* publisher = nullptr;
    Subscriber* subscriber = nullptr;
    Topic* topic = nullptr;
    BytesType* type = nullptr;
};

// 复现静态发现（EDP=STATIC）：REPRO_STATIC_XML=<path> 时启用
bool Setup(Ctx& c, const char* name) {
    auto* factory = DomainParticipantFactory::get_instance();
    DomainParticipantQos pqos = PARTICIPANT_QOS_DEFAULT;
    pqos.name(name);
    const char* static_xml = std::getenv("REPRO_STATIC_XML");
    if (static_xml != nullptr) {
        auto& dc = pqos.wire_protocol().builtin.discovery_config;
        dc.use_SIMPLE_EndpointDiscoveryProtocol = false;
        dc.use_STATIC_EndpointDiscoveryProtocol = true;
        std::string uri = std::string("file://") + static_xml;
        dc.static_edp_xml_config(uri.c_str());
    }
    // REPRO_SHM_XML=<profile.xml>：用 XML profile 建 participant（自定义 SHM 段，验证不崩 + 修大包突发）
    const char* shm_xml = std::getenv("REPRO_SHM_XML");
    if (shm_xml != nullptr) {
        if (factory->load_XML_profiles_file(shm_xml) != eprosima::fastdds::dds::RETCODE_OK) {
            std::printf("[repro] load xml failed\n");
            return false;
        }
        c.participant = factory->create_participant_with_profile(0, "detmw_participant");
        std::printf("[repro] participant via XML profile (SHM big segment)\n");
    } else {
        c.participant = factory->create_participant(0, pqos);
    }
    if (!c.participant) return false;
    c.type = new BytesType();
    TypeSupport ts(c.type);
    ts.register_type(c.participant);
    c.topic = c.participant->create_topic("burst", c.type->get_name(), TOPIC_QOS_DEFAULT);
    c.publisher = c.participant->create_publisher(PUBLISHER_QOS_DEFAULT);
    c.subscriber = c.participant->create_subscriber(SUBSCRIBER_QOS_DEFAULT);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::printf("usage: %s <pub|sub> <count> <size> [depth] [extra]\n", argv[0]);
        return 2;
    }
    std::string mode = argv[1];
    uint32_t count = std::strtoul(argv[2], nullptr, 10);
    uint32_t size = std::strtoul(argv[3], nullptr, 10);
    uint32_t depth = (argc > 4) ? std::strtoul(argv[4], nullptr, 10) : 100;

    Ctx c;
    if (!Setup(c, mode.c_str())) {
        std::printf("[repro] setup failed\n");
        return 1;
    }

    if (mode == "sub") {
        uint32_t timeoutS = (argc > 5) ? std::strtoul(argv[5], nullptr, 10) : 15;
        Listener listener(*c.type);
        DataReaderQos rqos = DATAREADER_QOS_DEFAULT;
        rqos.reliability().kind = RELIABLE_RELIABILITY_QOS;
        rqos.history().kind = KEEP_LAST_HISTORY_QOS;
        rqos.history().depth = depth;
        if (std::getenv("REPRO_TUNED") != nullptr) {  // 复现 detmw 调参 QoS
            rqos.history().depth = 1000;
            rqos.resource_limits().max_samples = 1000;
            rqos.resource_limits().max_samples_per_instance = 1000;
            rqos.reliable_reader_qos().times.initial_acknack_delay =
                eprosima::fastdds::dds::Duration_t{0, 0};
            rqos.reliable_reader_qos().times.heartbeat_response_delay =
                eprosima::fastdds::dds::Duration_t{0, 0};
        }
        if (std::getenv("REPRO_STATIC_XML") != nullptr) {  // 匹配 staticdiscovery.xml 端点 ID
            rqos.endpoint().entity_id = 2;
            rqos.endpoint().user_defined_id = 2;
        }
        if (!c.subscriber->create_datareader(c.topic, rqos, &listener)) {
            std::printf("[repro] reader create failed\n");
            return 1;
        }
        std::printf("[repro] sub waiting (expect %u x %uB)...\n", count, size);
        for (int i = 0; i < static_cast<int>(timeoutS * 10) && listener.recv.load() < count; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        // 排空在途
        std::this_thread::sleep_for(std::chrono::seconds(2));
        uint64_t recv = listener.recv.load();
        std::printf("[repro] sub recv=%llu/%u gap=%s -> %s\n",
                    static_cast<unsigned long long>(recv), count,
                    listener.gap.load() ? "YES" : "no",
                    (recv == count && !listener.gap.load()) ? "PASS (lossless)" : "LOSS/GAP");
        return (recv == count && !listener.gap.load()) ? 0 : 1;
    }

    // pub
    uint32_t delayUs = (argc > 5) ? std::strtoul(argv[5], nullptr, 10) : 0;
    uint32_t waitS = (argc > 6) ? std::strtoul(argv[6], nullptr, 10) : 3;
    DataWriterQos wqos = DATAWRITER_QOS_DEFAULT;
    wqos.reliability().kind = RELIABLE_RELIABILITY_QOS;
    wqos.history().kind = KEEP_LAST_HISTORY_QOS;
    wqos.history().depth = depth;
    if (std::getenv("REPRO_TUNED") != nullptr) {  // 复现 detmw 调参 QoS
        wqos.history().depth = 1000;
        wqos.resource_limits().max_samples = 1000;
        wqos.resource_limits().max_samples_per_instance = 1000;
        wqos.reliable_writer_qos().times.heartbeat_period =
            eprosima::fastdds::dds::Duration_t{0, 100 * 1000 * 1000};
        wqos.reliable_writer_qos().times.nack_response_delay =
            eprosima::fastdds::dds::Duration_t{0, 0};
    }
    if (std::getenv("REPRO_STATIC_XML") != nullptr) {  // 匹配 staticdiscovery.xml 端点 ID
        wqos.endpoint().entity_id = 1;
        wqos.endpoint().user_defined_id = 1;
    }
    DataWriter* writer = c.publisher->create_datawriter(c.topic, wqos);
    if (!writer) {
        std::printf("[repro] writer create failed\n");
        return 1;
    }
    std::printf("[repro] pub waiting %us...\n", waitS);
    std::this_thread::sleep_for(std::chrono::seconds(waitS));

    std::vector<uint8_t> pkt(size, 0);
    uint32_t fail = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < count; i++) {
        std::memcpy(pkt.data(), &i, 4);
        ReturnCode_t rc = writer->write(&pkt);
        if (rc != RETCODE_OK) fail++;
        if (delayUs > 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(delayUs));
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("[repro] pub wrote %u x %uB in %.1fms write_fail=%u\n", count, size, ms, fail);
    return 0;
}
