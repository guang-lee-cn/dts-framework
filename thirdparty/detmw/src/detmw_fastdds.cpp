#include "detmw_transport.h"

#include "bucket/BucketTransportDescriptor.h"  // 自研桶传输（DETMW_BUCKET=1 可选）
#include "detmw_fixed_type.h"
#include "log.h"

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

#include <fcntl.h>    // open：SHM 可用性探测
#include <sys/mman.h> // shm_open/shm_unlink：DETMW_BUCKET 预检
#include <unistd.h>

using namespace eprosima::fastdds::dds;
using namespace eprosima::fastdds::rtps;

namespace detmw {

namespace {

// 字节类型：payload 直接存原始字节（无序列化头）。可变长，非 plain（不支持 DataSharing 零拷贝）
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
    // plainSize = 0 → BytesType（可变长，空 vec）；>0 → FixedBytesType（定长预分配）
    RecvListener(detmw::recv_fn fn, void* ctx, uint32_t plainSize)
        : m_fn(fn), m_ctx(ctx), m_plainSize(plainSize) {}

    std::atomic<uint64_t> recvCount{0};  // 诊断：累计接收样本数

    void on_data_available(DataReader* reader) override {
        // 必须排空所有可用样本：FastDDS 不会为剩余样本重触发回调，
        // 只 take 一条会让数据堆在 reader history，被 KEEP_LAST 覆盖（高吞吐丢包根因）
        while (true) {
            SampleInfo info;
            // 替 create_data（移所有权给回调，零拷贝）；plain 类型预分配定长 buffer
            auto vec = std::make_unique<std::vector<uint8_t>>(m_plainSize);
            if (reader->take_next_sample(vec.get(), &info) != RETCODE_OK) {
                break;
            }
            if (info.valid_data) {
                recvCount.fetch_add(1, std::memory_order_relaxed);
                if (m_fn) {
                    m_fn(m_ctx, std::move(vec));  // 移交（回调消费后自动释放，reader 不 delete）
                }
            }
        }
    }

private:
    detmw::recv_fn m_fn;
    void* m_ctx;
    uint32_t m_plainSize = 0;  // 0 = BytesType
};

// 通信标识规则：endpoint -> DDS topic 名（订阅/发布同规则）
std::string MakeTopic(const detmw::endpoint& ep) {
    return ep.session_type + "_" + ep.session_inst + "_" + std::to_string(ep.msg_id);
}

struct FastDdsState {
    DomainParticipant* participant = nullptr;
    Publisher* publisher = nullptr;
    Subscriber* subscriber = nullptr;
    // per-topic 类型：TypeSupport（shared_ptr）是**唯一所有权**（participant m_types 共享计数）；
    // typeObjs 仅裸指针查询（不拥有，防双释放——曾 unique_ptr+TypeSupport 双持致析构 SEGV）
    std::unordered_map<std::string, TopicDataType*> typeObjs;
    std::unordered_map<std::string, TypeSupport> typeSupports;
    // 配置类型表：topic 名 → plain 定长（0 = BytesType）
    std::unordered_map<std::string, uint32_t> plainSizes;
    std::vector<Topic*> topics;
    std::vector<DataWriter*> writers;
    std::vector<DataReader*> readers;
    std::vector<std::unique_ptr<RecvListener>> listeners;
    std::unordered_map<std::string, uint64_t> writeCount;  // 诊断：每 topic 原始发布次数
};

// TODO(detmw-v2): 端点 entity_id/user_defined_id 由配置层（Communicator 解析 JSON）
//   提供。FastDDS transport 按 config 端点信息应用静态发现端点 ID。

TopicDataType* GetOrCreateType(FastDdsState* s, const std::string& topic_name) {
    auto it = s->typeObjs.find(topic_name);
    if (it != s->typeObjs.end()) {
        return it->second;
    }
    const auto pit = s->plainSizes.find(topic_name);
    TopicDataType* type = nullptr;
    if (pit != s->plainSizes.end() && pit->second > 0) {
        type = new FixedBytesType(pit->second);  // 定长 plain 通道（零拷贝候选）
    } else {
        type = new BytesType();  // 可变长兼容通道
    }
    // TypeSupport 唯一所有权（shared_ptr）；typeObjs 裸指针仅查询
    s->typeSupports.emplace(topic_name, TypeSupport(type));
    s->typeSupports[topic_name].register_type(s->participant);
    s->typeObjs.emplace(topic_name, type);
    dts::log::Info("[detmw] type up topic={} type={} plain={}", topic_name, type->get_name(),
                   type->is_plain(XCDR2_DATA_REPRESENTATION) ? 1 : 0);
    return type;
}

Topic* FindOrCreateTopic(FastDdsState* s, const std::string& topic_name) {
    for (auto* t : s->topics) {
        if (t->get_name() == topic_name) return t;
    }
    TopicDataType* type = GetOrCreateType(s, topic_name);
    Topic* t = s->participant->create_topic(topic_name, type->get_name(), TOPIC_QOS_DEFAULT);
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

// ---- QoS 调参（DETMW_QOS_* 环境变量，扫性能曲线用；进程级，所有 reader/writer 同一套）----
// 蓝本参考 quality/test/fastdds_burst_repro.cpp 的 REPRO_TUNED 块。
// 注意：SHM 段大小不可调（Bug 8 自定义 SHM 析构 SEGV），transport 维度仅 SHM/UDP 二选一。
struct QosSettings {
    bool reliable = true;         // DETMW_QOS_RELIABILITY=reliable|best_effort
    bool keepLast = true;         // DETMW_QOS_HISTORY=keep_last|keep_all
    uint32_t depth = 1000;        // DETMW_QOS_DEPTH
    bool transientLocal = false;  // DETMW_QOS_DURABILITY=transient_local
    int32_t maxSamples = -1;      // DETMW_QOS_MAX_SAMPLES（-1=不限）
};

const QosSettings& LoadQos() {
    static const QosSettings q = [] {
        QosSettings s;
        if (const char* v = std::getenv("DETMW_QOS_RELIABILITY")) {
            s.reliable = std::strcmp(v, "best_effort") != 0;
        }
        if (const char* v = std::getenv("DETMW_QOS_HISTORY")) {
            s.keepLast = std::strcmp(v, "keep_all") != 0;
        }
        if (const char* v = std::getenv("DETMW_QOS_DEPTH")) {
            s.depth = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
        }
        if (const char* v = std::getenv("DETMW_QOS_DURABILITY")) {
            s.transientLocal = std::strcmp(v, "transient_local") == 0;
        }
        if (const char* v = std::getenv("DETMW_QOS_MAX_SAMPLES")) {
            s.maxSamples = static_cast<int32_t>(std::strtol(v, nullptr, 10));
        }
        dts::log::Info("[detmw] QoS reliability={} history={} depth={} transient_local={} max_samples={}",
                       s.reliable ? "reliable" : "best_effort", s.keepLast ? "keep_last" : "keep_all",
                       s.depth, s.transientLocal ? 1 : 0, s.maxSamples);
        return s;
    }();
    return q;
}

}  // namespace

// FastDDS 传输实现：TransportInterface 的 FastDDS 适配（D10 唯一切换点）
class FastDdsTransport : public TransportInterface {
public:
    // participant 起立成功（构造未提前失败返回）
    bool Ready() const { return m_state != nullptr && m_state->participant != nullptr; }

    FastDdsTransport(int domain_id, const char* process_name, const char* static_xml_path,
                     const std::unordered_map<std::string, uint32_t>& plain_topics)
        : m_state(new FastDdsState()) {
        m_state->plainSizes = plain_topics;  // topic → 定长 plain（0/缺省 = BytesType）
        auto* factory = DomainParticipantFactory::get_instance();
        DomainParticipantQos pqos = PARTICIPANT_QOS_DEFAULT;
        pqos.name(process_name);

        // 诊断：DETMW_UDP_ONLY=1 强制 UDP（禁 SHM 内置传输），对比大包突发
        if (std::getenv("DETMW_UDP_ONLY") != nullptr) {
            auto udp = std::make_shared<UDPv4TransportDescriptor>();
            pqos.transport().use_builtin_transports = false;
            pqos.transport().user_transports.push_back(udp);
            dts::log::Warn("[detmw] UDP-only transport forced (DETMW_UDP_ONLY)");
        }
        // 自研桶传输（DETMW_BUCKET=1）：共享内存环替代内置 SHM 数据面
        // （绕开 FastDDS 3.6 Bug 8 析构 UAF + 512KB 段容量限制——上游 3.6.2/master 均未修，
        //   见 docs/design/fastdds-upgrade-assessment.md；段容量 DETMW_BUCKET_CAP_MB，默认 64MB）
        // 商用语义：显式请求的确定性传输不可用 = 配置错误 → fail-fast（good()=false，Run 失败）
        if (std::getenv("DETMW_BUCKET") != nullptr) {
            // 预检：POSIX shm 可用性（探测段创建即删；失败 = 桶无法创建，拒绝启动）
            const char* kBucketProbe = "/dts_detmw_bucket_probe";
            const int pfd = ::shm_open(kBucketProbe, O_CREAT | O_EXCL | O_RDWR, 0600);
            if (pfd < 0) {
                dts::log::Error("[detmw] DETMW_BUCKET=1 but POSIX shm unavailable (shm_open failed): "
                                "bucket transport cannot start (fail-fast)");
                return;  // participant 未建 → good()=false
            }
            ::close(pfd);
            ::shm_unlink(kBucketProbe);
            auto bucket_desc = std::make_shared<bucket::BucketTransportDescriptor>();
            if (const char* cap = std::getenv("DETMW_BUCKET_CAP_MB")) {
                bucket_desc->segment_size(
                    static_cast<uint32_t>(std::strtoul(cap, nullptr, 10)) * 1024 * 1024);
            }
            pqos.transport().use_builtin_transports = false;
            pqos.transport().user_transports.push_back(bucket_desc);  // 数据面：桶（大段、无 UAF）
            pqos.transport().user_transports.push_back(
                std::make_shared<UDPv4TransportDescriptor>());        // 发现面：UDP（EDP 组播/单播）
            dts::log::Warn("[detmw] bucket transport enabled (segment={}MB, discovery=UDP)",
                           bucket_desc->segment_size() / (1024 * 1024));
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
                dts::log::Error("[detmw] static discovery xml invalid: {}", static_xml_path);
                return;
            }
        } else {
            dts::log::Info("[detmw] dynamic EDP (SIMPLE)");
        }

        m_state->participant = factory->create_participant(domain_id, pqos);
        if (!m_state->participant) {
            dts::log::Error("[detmw] participant create failed (domain={})", domain_id);
            return;
        }
        // 类型按 topic 懒注册（GetOrCreateType，BytesType/FixedBytesType）
        m_state->publisher = m_state->participant->create_publisher(PUBLISHER_QOS_DEFAULT);
        m_state->subscriber = m_state->participant->create_subscriber(SUBSCRIBER_QOS_DEFAULT);
        // 静默降级告警：/dev/shm 不可写 → FastDDS 内置 SHM 传输注册失败（自身 Error 日志后
        // 继续 UDP）。32K 大包在受限网络（UDP 分片/小 MTU）下可靠投递无保证，必须显式暴露。
        // （沙箱/容器常见；生产 /dev/shm 应可用，或显式 DETMW_UDP_ONLY=1 承认 UDP 路径）
        // 探测用实际 open（access() 在部分沙箱/容器返回允许但 open 失败，不可靠）
        const char* kShmProbe = "/dev/shm/.dts_shm_probe";
        const int probeFd = ::open(kShmProbe, O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
        if (probeFd < 0 && std::getenv("DETMW_UDP_ONLY") == nullptr) {
            dts::log::Warn("[detmw] /dev/shm not writable: FastDDS SHM 传输将失败并降级 UDP；"
                           "32K 大包可靠投递依赖传输上限（Bug 7 环境依赖），建议修复 /dev/shm 权限"
                           "或显式 DETMW_UDP_ONLY=1 确认走 UDP");
        } else {
            ::close(probeFd);
            ::unlink(kShmProbe);
        }
        dts::log::Info("[detmw] participant up (name={} static_edp={})", process_name, static_xml_path);
    }

    ~FastDdsTransport() override {
        if (!m_state || !m_state->participant) {
            delete m_state;
            return;
        }
        // 诊断：reader 累计接收 + writer 原始发布次数
        for (size_t i = 0; i < m_state->readers.size(); ++i) {
            dts::log::Info("[detmw] reader[{}] recv_total={}", i, m_state->listeners[i]->recvCount.load());
        }
        for (const auto& kv : m_state->writeCount) {
            dts::log::Info("[detmw] writer[{}] write_total={}", kv.first, kv.second);
        }
        DomainParticipantFactory::get_instance()->delete_participant(m_state->participant);
        // typeObjs/typeSupports 随 m_state 析构自动释放
        delete m_state;
    }

    int CreateReader(const endpoint& ep, recv_fn fn, void* ctx) override {
        if (!m_state->participant) {
            dts::log::Error("[detmw] CreateReader: participant not ready");
            return -1;
        }
        const std::string topic = MakeTopic(ep);
        Topic* t = FindOrCreateTopic(m_state, topic);
        if (!t) {
            dts::log::Error("[detmw] topic create failed: {}", topic);
            return -1;
        }
        const auto pit = m_state->plainSizes.find(topic);
        const uint32_t plainSize = (pit != m_state->plainSizes.end()) ? pit->second : 0;
        auto listener = std::make_unique<RecvListener>(fn, ctx, plainSize);
        const QosSettings& qos = LoadQos();
        DataReaderQos rqos = DATAREADER_QOS_DEFAULT;
        rqos.reliability().kind = qos.reliable ? RELIABLE_RELIABILITY_QOS : BEST_EFFORT_RELIABILITY_QOS;
        rqos.history().kind = qos.keepLast ? KEEP_LAST_HISTORY_QOS : KEEP_ALL_HISTORY_QOS;
        rqos.history().depth = qos.depth;
        if (qos.transientLocal) {
            rqos.durability().kind = TRANSIENT_LOCAL_DURABILITY_QOS;
        }
        if (qos.maxSamples >= 0) {
            rqos.resource_limits().max_samples = qos.maxSamples;
        }
        DataReader* reader = m_state->subscriber->create_datareader(t, rqos, listener.get());
        if (!reader) {
            dts::log::Error("[detmw] reader create failed: {}", topic);
            return -1;
        }
        m_state->listeners.push_back(std::move(listener));
        m_state->readers.push_back(reader);
        dts::log::Info("[detmw] subscribed topic={}", topic);
        return 0;
    }

    int CreateWriter(const endpoint& ep) override {
        if (!m_state->participant) {
            dts::log::Error("[detmw] CreateWriter: participant not ready");
            return -1;
        }
        const std::string topic = MakeTopic(ep);
        if (FindWriter(m_state, topic) != nullptr) {
            return 0;  // 已建
        }
        Topic* t = FindOrCreateTopic(m_state, topic);
        if (!t) {
            dts::log::Error("[detmw] topic create failed: {}", topic);
            return -1;
        }
        const QosSettings& qos = LoadQos();
        DataWriterQos wqos = DATAWRITER_QOS_DEFAULT;
        wqos.reliability().kind = qos.reliable ? RELIABLE_RELIABILITY_QOS : BEST_EFFORT_RELIABILITY_QOS;
        wqos.history().kind = qos.keepLast ? KEEP_LAST_HISTORY_QOS : KEEP_ALL_HISTORY_QOS;
        wqos.history().depth = qos.depth;
        if (qos.transientLocal) {
            wqos.durability().kind = TRANSIENT_LOCAL_DURABILITY_QOS;
        }
        if (qos.maxSamples >= 0) {
            wqos.resource_limits().max_samples = qos.maxSamples;
        }
        DataWriter* w = m_state->publisher->create_datawriter(t, wqos);
        if (!w) {
            dts::log::Error("[detmw] writer create failed: {}", topic);
            return -1;
        }
        m_state->writers.push_back(w);
        dts::log::Info("[detmw] writer up topic={}", topic);
        return 0;
    }

    int Send(const endpoint& ep, const uint8_t* data, uint32_t len) override {
        if (!m_state->participant) {
            dts::log::Error("[detmw] Send: participant not ready");
            return -1;
        }
        const std::string topic = MakeTopic(ep);
        DataWriter* w = FindWriter(m_state, topic);
        if (!w) {
            dts::log::Error("[detmw] writer not precreated: {}", topic);
            return -1;
        }
        // 定长 plain 通道：严格等长（FixedBytesType 语义），超长/不足拒绝（配置错误）
        const auto pit = m_state->plainSizes.find(topic);
        const bool plain = pit != m_state->plainSizes.end() && pit->second > 0;
        if (plain && len != pit->second) {
            dts::log::Error("[detmw] plain topic {} size mismatch: {} != {} (strict fixed size)",
                            topic, len, pit->second);
            return -1;
        }

        // 零拷贝路径（DataSharing，需 SHM 传输）：loan_sample 借共享内存 buffer 直写。
        // 不可用（无 SHM/非 DataSharing）→ 回退普通 write（多一次拷贝，功能不回归）。
        if (plain) {
            void* loan = nullptr;
            if (w->loan_sample(loan) == RETCODE_OK) {
                std::memcpy(loan, data, len);
                ReturnCode_t rc = w->write(loan);  // write 后 loan 归 FastDDS 管理
                if (rc != RETCODE_OK) {
                    dts::log::Error("[detmw] loan write failed: {}", topic);
                    w->discard_loan(loan);
                    return -1;
                }
                m_state->writeCount[topic]++;
                dts::log::Info("[detmw] published topic={} len={} (loan/zero-copy)", topic, len);
                return 0;
            }
            dts::log::Debug("[detmw] loan unavailable (no DataSharing/SHM), fallback copy: {}",
                            topic);
        }

        std::vector<uint8_t> payload(data, data + len);
        ReturnCode_t rc = w->write(&payload);
        if (rc != RETCODE_OK) {
            dts::log::Error("[detmw] write failed: {}", topic);
            return -1;
        }
        m_state->writeCount[topic]++;
        dts::log::Info("[detmw] published topic={} len={}", topic, len);
        return 0;
    }

private:
    FastDdsState* m_state;
};

// 传输工厂：detmw 公共层（Communicator）调用，隐藏 FastDDS 实现细节
// plain_topics：topic 名 → 定长（>0 = FixedBytesType plain 通道；缺省 = BytesType）
// 构造失败（participant 未起立，如 DETMW_BUCKET 预检 fail-fast）→ 返回 nullptr，
// Communicator::good()=false → Run 装配失败（商用语义：显式请求的传输不可用 = 配置错误）
std::unique_ptr<TransportInterface> CreateFastDdsTransport(
    int domain_id, const char* process_name, const char* static_xml_path,
    const std::unordered_map<std::string, uint32_t>& plain_topics) {
    auto t = std::make_unique<FastDdsTransport>(domain_id, process_name, static_xml_path,
                                                plain_topics);
    return t->Ready() ? std::unique_ptr<TransportInterface>(std::move(t)) : nullptr;
}

}  // namespace detmw
