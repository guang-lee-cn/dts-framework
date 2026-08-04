#include "detmw_transport.h"

#include <spdlog/spdlog.h>

#include <fastdds/dds/core/ReturnCode.hpp>
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/DataReaderListener.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/TopicDataType.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>
#include <fastdds/rtps/transport/UDPv4TransportDescriptor.hpp>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using namespace eprosima::fastdds::dds;
using namespace eprosima::fastdds::rtps;

namespace detmw {

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
    RecvListener(BytesType& type, detmw::recv_fn fn, void* ctx)
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
    detmw::recv_fn m_fn;
    void* m_ctx;
};

// 通信标识规则：endpoint -> DDS topic 名（订阅/发布同规则）
std::string MakeTopic(const detmw::endpoint& ep) {
    return ep.session_type + "_" + ep.session_inst + "_" + std::to_string(ep.msg_id);
}

struct FastDdsState {
    DomainParticipant* participant = nullptr;
    Publisher* publisher = nullptr;
    Subscriber* subscriber = nullptr;
    BytesType* type = nullptr;
    TypeSupport typeSupport;
    std::vector<Topic*> topics;
    std::vector<DataWriter*> writers;
    std::vector<DataReader*> readers;
    std::vector<std::unique_ptr<RecvListener>> listeners;
    std::unordered_map<std::string, uint64_t> writeCount;  // 诊断：每 topic 原始发布次数
};

// TODO(detmw-v2): 端点 entity_id/user_defined_id 由配置层（Communicator 解析 JSON）
//   提供。FastDDS transport 按 config 端点信息应用静态发现端点 ID。

Topic* FindOrCreateTopic(FastDdsState* s, const std::string& topic_name) {
    for (auto* t : s->topics) {
        if (t->get_name() == topic_name) return t;
    }
    Topic* t = s->participant->create_topic(topic_name, s->type->get_name(), TOPIC_QOS_DEFAULT);
    if (t) {
        s->topics.push_back(t);
    }
    return t;
}

DataWriter* FindWriter(FastDdsState* s, const std::string& topic_name) {
    for (auto* w : s->writers) {
        if (w->get_topic()->get_name() == topic_name) return w;
    }
    return nullptr;
}

}  // namespace

// FastDDS 传输实现：TransportInterface 的 FastDDS 适配（D10 唯一切换点）
class FastDdsTransport : public TransportInterface {
public:
    FastDdsTransport(int domain_id, const char* process_name, const char* static_xml_path)
        : m_state(new FastDdsState()) {
        auto* factory = DomainParticipantFactory::get_instance();
        DomainParticipantQos pqos = PARTICIPANT_QOS_DEFAULT;
        pqos.name(process_name);

        // 诊断：DETMW_UDP_ONLY=1 强制 UDP（禁 SHM 内置传输），对比大包突发
        if (std::getenv("DETMW_UDP_ONLY") != nullptr) {
            auto udp = std::make_shared<UDPv4TransportDescriptor>();
            pqos.transport().use_builtin_transports = false;
            pqos.transport().user_transports.push_back(udp);
            spdlog::warn("[detmw] UDP-only transport forced (DETMW_UDP_ONLY)");
        }
        // 发现方案：默认动态 EDP（SIMPLE）；DETMW_STATIC=1 显式切回静态 EDP
        const bool use_static = std::getenv("DETMW_STATIC") != nullptr;
        if (use_static) {
            auto& dc = pqos.wire_protocol().builtin.discovery_config;
            dc.use_SIMPLE_EndpointDiscoveryProtocol = false;
            dc.use_STATIC_EndpointDiscoveryProtocol = true;
            std::string xml_uri = std::string("file://") + static_xml_path;
            dc.static_edp_xml_config(xml_uri.c_str());
            if (factory->check_xml_static_discovery(xml_uri) != RETCODE_OK) {
                spdlog::error("[detmw] static discovery xml invalid: {}", static_xml_path);
                return;
            }
        } else {
            spdlog::info("[detmw] dynamic EDP (SIMPLE)");
        }

        m_state->participant = factory->create_participant(domain_id, pqos);
        if (!m_state->participant) {
            spdlog::error("[detmw] participant create failed (domain={})", domain_id);
            return;
        }
        m_state->type = new BytesType();
        m_state->typeSupport = TypeSupport(m_state->type);
        m_state->typeSupport.register_type(m_state->participant);
        m_state->publisher = m_state->participant->create_publisher(PUBLISHER_QOS_DEFAULT);
        m_state->subscriber = m_state->participant->create_subscriber(SUBSCRIBER_QOS_DEFAULT);
        spdlog::info("[detmw] participant up (name={} static_edp={})", process_name, static_xml_path);
    }

    ~FastDdsTransport() override {
        if (!m_state || !m_state->participant) {
            delete m_state;
            return;
        }
        // 诊断：reader 累计接收 + writer 原始发布次数
        for (size_t i = 0; i < m_state->readers.size(); ++i) {
            spdlog::info("[detmw] reader[{}] recv_total={}", i, m_state->listeners[i]->recvCount.load());
        }
        for (const auto& kv : m_state->writeCount) {
            spdlog::info("[detmw] writer[{}] write_total={}", kv.first, kv.second);
        }
        DomainParticipantFactory::get_instance()->delete_participant(m_state->participant);
        // typeSupport 值成员（shared_ptr）析构自动释放 m_state->type
        delete m_state;
    }

    int CreateReader(const endpoint& ep, recv_fn fn, void* ctx) override {
        if (!m_state->participant) {
            spdlog::error("[detmw] CreateReader: participant not ready");
            return -1;
        }
        const std::string topic = MakeTopic(ep);
        Topic* t = FindOrCreateTopic(m_state, topic);
        if (!t) {
            spdlog::error("[detmw] topic create failed: {}", topic);
            return -1;
        }
        auto listener = std::make_unique<RecvListener>(*m_state->type, fn, ctx);
        DataReaderQos rqos = DATAREADER_QOS_DEFAULT;
        rqos.reliability().kind = RELIABLE_RELIABILITY_QOS;
        rqos.history().kind = KEEP_LAST_HISTORY_QOS;
        rqos.history().depth = 1000;  // 加深在途容量（不动可靠 timing，避免丢包）
        DataReader* reader = m_state->subscriber->create_datareader(t, rqos, listener.get());
        if (!reader) {
            spdlog::error("[detmw] reader create failed: {}", topic);
            return -1;
        }
        m_state->listeners.push_back(std::move(listener));
        m_state->readers.push_back(reader);
        spdlog::info("[detmw] subscribed topic={}", topic);
        return 0;
    }

    int CreateWriter(const endpoint& ep) override {
        if (!m_state->participant) {
            spdlog::error("[detmw] CreateWriter: participant not ready");
            return -1;
        }
        const std::string topic = MakeTopic(ep);
        if (FindWriter(m_state, topic) != nullptr) {
            return 0;  // 已建
        }
        Topic* t = FindOrCreateTopic(m_state, topic);
        if (!t) {
            spdlog::error("[detmw] topic create failed: {}", topic);
            return -1;
        }
        DataWriterQos wqos = DATAWRITER_QOS_DEFAULT;
        wqos.reliability().kind = RELIABLE_RELIABILITY_QOS;
        wqos.history().kind = KEEP_LAST_HISTORY_QOS;
        wqos.history().depth = 1000;
        DataWriter* w = m_state->publisher->create_datawriter(t, wqos);
        if (!w) {
            spdlog::error("[detmw] writer create failed: {}", topic);
            return -1;
        }
        m_state->writers.push_back(w);
        spdlog::info("[detmw] writer up topic={}", topic);
        return 0;
    }

    int Send(const endpoint& ep, const uint8_t* data, uint32_t len) override {
        if (!m_state->participant) {
            spdlog::error("[detmw] Send: participant not ready");
            return -1;
        }
        const std::string topic = MakeTopic(ep);
        DataWriter* w = FindWriter(m_state, topic);
        if (!w) {
            spdlog::error("[detmw] writer not precreated: {}", topic);
            return -1;
        }
        std::vector<uint8_t> payload(data, data + len);
        ReturnCode_t rc = w->write(&payload);
        if (rc != RETCODE_OK) {
            spdlog::error("[detmw] write failed: {}", topic);
            return -1;
        }
        m_state->writeCount[topic]++;
        spdlog::info("[detmw] published topic={} len={}", topic, len);
        return 0;
    }

private:
    FastDdsState* m_state;
};

// 传输工厂：detmw 公共层（Communicator）调用，隐藏 FastDDS 实现细节
std::unique_ptr<TransportInterface> CreateFastDdsTransport(int domain_id,
                                                           const char* process_name,
                                                           const char* static_xml_path) {
    return std::make_unique<FastDdsTransport>(domain_id, process_name, static_xml_path);
}

}  // namespace detmw
