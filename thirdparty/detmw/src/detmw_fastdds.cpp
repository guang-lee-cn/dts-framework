#include <spdlog/spdlog.h>
#include "detmw_fastdds.h"

#include <fastdds/dds/core/ReturnCode.hpp>
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/log/Log.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/DataReaderListener.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/TopicDataType.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>
#include <fastdds/rtps/transport/UDPv4TransportDescriptor.hpp>
#include <fastdds/rtps/transport/shared_mem/SharedMemTransportDescriptor.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace eprosima::fastdds::dds;

namespace {

// 字节类型：payload 直接存原始字节（无序列化头）
class BytesType : public TopicDataType {
public:
    BytesType() {
        set_name("detmw::Bytes");
        max_serialized_type_size = 64 * 1024;  // detmw 消息上限 ≤32K，留余量
    }

    bool serialize(const void* const_data, eprosima::fastdds::rtps::SerializedPayload_t& payload,
                   DataRepresentationId_t) override {
        const auto* vec = static_cast<const std::vector<uint8_t>*>(const_data);
        payload.reserve(static_cast<uint32_t>(vec->size()));
        std::memcpy(payload.data, vec->data(), vec->size());
        payload.length = static_cast<uint32_t>(vec->size());
        return true;
    }

    bool deserialize(eprosima::fastdds::rtps::SerializedPayload_t& payload, void* data) override {
        auto* vec = static_cast<std::vector<uint8_t>*>(data);
        vec->assign(payload.data, payload.data + payload.length);
        return true;
    }

    uint32_t calculate_serialized_size(const void* data, DataRepresentationId_t) override {
        const auto* vec = static_cast<const std::vector<uint8_t>*>(data);
        return static_cast<uint32_t>(vec->size());
    }

    void* create_data() override { return new std::vector<uint8_t>(); }
    void delete_data(void* data) override { delete static_cast<std::vector<uint8_t>*>(data); }

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

class RecvListener : public DataReaderListener {
public:
    RecvListener(BytesType& type, detmw_recv_fn fn, void* ctx)
        : m_type(type), m_fn(fn), m_ctx(ctx) {}

    std::atomic<uint64_t> recvCount{0};  // 诊断：累计接收样本数

    void on_data_available(DataReader* reader) override {
        // 必须排空所有可用样本：FastDDS 不会为剩余样本重触发回调，
        // 只 take 一条会让数据堆在 reader history，被 KEEP_LAST 覆盖（高吞吐丢包根因）
        while (true) {
            SampleInfo info;
            void* data = m_type.create_data();
            if (reader->take_next_sample(data, &info) != RETCODE_OK) {
                m_type.delete_data(data);
                break;
            }
            if (info.valid_data) {
                auto* vec = static_cast<std::vector<uint8_t>*>(data);
                recvCount.fetch_add(1, std::memory_order_relaxed);
                if (m_fn) {
                    m_fn(m_ctx, vec->data(), static_cast<uint32_t>(vec->size()));
                }
            }
            m_type.delete_data(data);
        }
    }

private:
    BytesType& m_type;
    detmw_recv_fn m_fn;
    void* m_ctx;
};

struct Adapter {
    DomainParticipant* m_participant = nullptr;
    Publisher* m_publisher = nullptr;
    Subscriber* m_subscriber = nullptr;
    BytesType* m_type = nullptr;
    TypeSupport m_typeSupport;
    std::vector<Topic*> m_topics;
    std::vector<DataWriter*> m_writers;
    std::vector<DataReader*> m_readers;
    std::vector<std::unique_ptr<RecvListener>> m_listeners;
    std::unordered_map<std::string, uint64_t> m_writeCount;  // 诊断：每 topic 原始发布次数
};

Adapter* AsAdapter(void* h) { return static_cast<Adapter*>(h); }

// 端点 QoS：静态发现须显式 entity_id/user_defined_id（与生成的 staticdiscovery.xml 一致）
void ApplyEndpointId(DataWriterQos& qos, uint16_t user_defined_id, uint16_t entity_id) {
    qos.endpoint().user_defined_id = static_cast<int16_t>(user_defined_id);
    qos.endpoint().entity_id = static_cast<int16_t>(entity_id);
}

void ApplyEndpointId(DataReaderQos& qos, uint16_t user_defined_id, uint16_t entity_id) {
    qos.endpoint().user_defined_id = static_cast<int16_t>(user_defined_id);
    qos.endpoint().entity_id = static_cast<int16_t>(entity_id);
}

Topic* FindOrCreateTopic(Adapter* a, const char* topic_name) {
    for (auto* t : a->m_topics) {
        if (t->get_name() == topic_name) return t;
    }
    Topic* t = a->m_participant->create_topic(topic_name, a->m_type->get_name(), TOPIC_QOS_DEFAULT);
    if (t) {
        a->m_topics.push_back(t);
    }
    return t;
}

DataWriter* FindWriter(Adapter* a, const char* topic_name) {
    for (auto* w : a->m_writers) {
        if (w->get_topic()->get_name() == topic_name) return w;
    }
    return nullptr;
}

}  // namespace

extern "C" {

void* detmw_fastdds_create(int domain_id, const char* process_name, const char* static_xml_path) {
    auto* a = new Adapter();
    auto* factory = DomainParticipantFactory::get_instance();

    DomainParticipantQos pqos = PARTICIPANT_QOS_DEFAULT;
    pqos.name(process_name);
    // 诊断：DETMW_UDP_ONLY=1 强制 UDP（禁 SHM 内置传输），对比大包突发
    // 注：默认用内置 SHM（段仅 512KB，32K 消息只能缓冲 ~16 条，紧突发会丢）。
    //   曾尝试自定义 SHM 描述符加大段（32MB）修 32K 突发，但 FastDDS 3.6 任何自定义
    //   SharedMemTransportDescriptor 都会在析构时崩（SHM WatchTask use-after-free）——故回退内置。
    //   32K 紧突发丢 = FastDDS 3.6 内置 SHM 段过小，需升级 FastDDS 或等修复。
    if (std::getenv("DETMW_UDP_ONLY") != nullptr) {
        auto udp = std::make_shared<eprosima::fastdds::rtps::UDPv4TransportDescriptor>();
        pqos.transport().use_builtin_transports = false;
        pqos.transport().user_transports.push_back(udp);
        spdlog::warn("[detmw] UDP-only transport forced (DETMW_UDP_ONLY)");
    }
    // 发现方案：默认动态 EDP（SIMPLE，免 staticdiscovery.xml）；DETMW_STATIC=1 显式切回静态 EDP
    const bool use_static = std::getenv("DETMW_STATIC") != nullptr;
    if (use_static) {
        auto& dc = pqos.wire_protocol().builtin.discovery_config;
        dc.use_SIMPLE_EndpointDiscoveryProtocol = false;
        dc.use_STATIC_EndpointDiscoveryProtocol = true;
        std::string xml_uri = std::string("file://") + static_xml_path;
        dc.static_edp_xml_config(xml_uri.c_str());

        std::string xml_file = xml_uri;
        if (factory->check_xml_static_discovery(xml_file) != RETCODE_OK) {
            spdlog::error("[detmw] static discovery xml invalid: {}", static_xml_path);
            delete a;
            return nullptr;
        }
    } else {
        spdlog::info("[detmw] dynamic EDP (SIMPLE)");
    }

    a->m_participant = factory->create_participant(domain_id, pqos);
    if (!a->m_participant) {
        spdlog::error("[detmw] participant create failed (domain={})", domain_id);
        delete a;
        return nullptr;
    }
    // 先注册类型，再创建发布/订阅实体
    a->m_type = new BytesType();
    a->m_typeSupport = TypeSupport(a->m_type);   // shared_ptr 拥有 m_type
    a->m_typeSupport.register_type(a->m_participant);
    a->m_publisher = a->m_participant->create_publisher(PUBLISHER_QOS_DEFAULT);
    a->m_subscriber = a->m_participant->create_subscriber(SUBSCRIBER_QOS_DEFAULT);
    spdlog::info("[detmw] participant up (name={} static_edp={})", process_name, static_xml_path);
    return a;
}

int detmw_fastdds_subscribe(void* h, const char* topic_name,
                            uint16_t user_defined_id, uint16_t entity_id,
                            detmw_recv_fn fn, void* ctx) {
    auto* a = AsAdapter(h);
    Topic* t = FindOrCreateTopic(a, topic_name);

    auto listener = std::make_unique<RecvListener>(*a->m_type, fn, ctx);
    DataReaderQos rqos = DATAREADER_QOS_DEFAULT;
    rqos.reliability().kind = RELIABLE_RELIABILITY_QOS;
    rqos.history().kind = KEEP_LAST_HISTORY_QOS;
    rqos.history().depth = 1000;  // 加深在途容量（不动可靠 timing，避免丢包）
    ApplyEndpointId(rqos, user_defined_id, entity_id);
    DataReader* reader = a->m_subscriber->create_datareader(t, rqos, listener.get());
    if (!reader) {
        spdlog::error("[detmw] reader create failed: {}", topic_name);
        return -1;
    }
    a->m_listeners.push_back(std::move(listener));
    a->m_readers.push_back(reader);
    spdlog::info("[detmw] subscribed topic={} uid={} eid={}", topic_name, user_defined_id, entity_id);
    return 0;
}

int detmw_fastdds_create_writer(void* h, const char* topic_name,
                                uint16_t user_defined_id, uint16_t entity_id) {
    auto* a = AsAdapter(h);
    if (FindWriter(a, topic_name) != nullptr) {
        return 0;  // 已建
    }
    Topic* t = FindOrCreateTopic(a, topic_name);
    // 可靠传输：reliable + KEEP_LAST，保证发现后不丢（对齐 S 级可靠需求）
    DataWriterQos wqos = DATAWRITER_QOS_DEFAULT;
    wqos.reliability().kind = RELIABLE_RELIABILITY_QOS;
    wqos.history().kind = KEEP_LAST_HISTORY_QOS;
    wqos.history().depth = 1000;  // 加深在途容量（不动可靠 timing，避免丢包）
    ApplyEndpointId(wqos, user_defined_id, entity_id);
    DataWriter* w = a->m_publisher->create_datawriter(t, wqos);
    if (!w) {
        spdlog::error("[detmw] writer create failed: {}", topic_name);
        return -1;
    }
    a->m_writers.push_back(w);
    spdlog::info("[detmw] writer up topic={} uid={} eid={}", topic_name, user_defined_id, entity_id);
    return 0;
}

int detmw_fastdds_publish(void* h, const char* topic_name, const uint8_t* data, uint32_t len) {
    auto* a = AsAdapter(h);
    DataWriter* w = FindWriter(a, topic_name);
    if (!w) {
        spdlog::error("[detmw] writer not precreated: {}", topic_name);
        return -1;
    }
    std::vector<uint8_t> payload(data, data + len);
    ReturnCode_t rc = w->write(&payload);
    if (rc != RETCODE_OK) {
        spdlog::error("[detmw] write failed: {}", topic_name);
        return -1;
    }
    a->m_writeCount[topic_name]++;
    spdlog::info("[detmw] published topic={} len={}", topic_name, len);
    return 0;
}

void detmw_fastdds_destroy(void* h) {
    auto* a = AsAdapter(h);
    if (!a) return;
    // 诊断：每个 reader 累计接收样本数 + 每 topic 原始发布次数
    for (size_t i = 0; i < a->m_readers.size(); ++i) {
        spdlog::info("[detmw] reader[{}] recv_total={}", i, a->m_listeners[i]->recvCount.load());
    }
    for (const auto& kv : a->m_writeCount) {
        spdlog::info("[detmw] writer[{}] write_total={}", kv.first, kv.second);
    }
    auto* factory = DomainParticipantFactory::get_instance();
    if (a->m_participant) {
        // DDS 资源交给 Fast-DDS 释放：delete_participant 自动清理全部端点
        factory->delete_participant(a->m_participant);
    }
    // m_typeSupport 是值成员（shared_ptr<TopicDataType>），析构自动释放 m_type；
    // detmw 只释放自己 new 的 Adapter
    delete a;
}

}  // extern "C"
