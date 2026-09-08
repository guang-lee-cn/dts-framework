#include "kafka_client.h"

#include <chrono>
#include <cstring>

#include "log.h"

namespace web {

using dts::log::Error;
using dts::log::Info;
using dts::log::Warn;

namespace {

constexpr int kStartTimeoutS = 5;     // metadata 连通性判定窗口
constexpr int kFlushTimeoutMs = 3000; // 下电 flush 上限

// 错误日志收敛：同一错误码只打一次，避免 broker 断连时刷屏
void LogOnce(int err, const char* where) {
    static int s_lastErr = 0;
    if (err != s_lastErr) {
        s_lastErr = err;
        Warn("[web:kafka] {} err={} ({})", where, err, rd_kafka_err2str(static_cast<rd_kafka_resp_err_t>(err)));
    }
}

}  // namespace

// ---- KafkaProducer ----

KafkaProducer::KafkaProducer(const std::string& brokers, const std::string& topic)
    : m_brokers(brokers), m_topic(topic) {}

KafkaProducer::~KafkaProducer() {
    Shutdown();
}

int KafkaProducer::Start() {
    char errStr[256] = {0};
    rd_kafka_conf_t* conf = rd_kafka_conf_new();
    rd_kafka_conf_set(conf, "bootstrap.servers", m_brokers.c_str(), errStr, sizeof(errStr));
    rd_kafka_conf_set(conf, "message.timeout.ms", "5000", errStr, sizeof(errStr));
    rd_kafka_conf_set_dr_msg_cb(conf, &KafkaProducer::OnDelivery);

    m_rk = rd_kafka_new(RD_KAFKA_PRODUCER, conf, errStr, sizeof(errStr));
    if (m_rk == nullptr) {
        rd_kafka_conf_destroy(conf);
        Error("[web:kafka] producer create failed: {}", errStr);
        return -1;
    }
    m_rkt = rd_kafka_topic_new(m_rk, m_topic.c_str(), nullptr);
    if (m_rkt == nullptr) {
        Error("[web:kafka] topic new failed: {}", m_topic);
        return -1;
    }

    // 连通性判定：metadata 请求（fail-fast，不在就退，不静默运行）
    const rd_kafka_metadata_t* md = nullptr;
    rd_kafka_resp_err_t rc = rd_kafka_metadata(m_rk, 0, nullptr, &md, kStartTimeoutS * 1000);
    if (rc != RD_KAFKA_RESP_ERR_NO_ERROR) {
        Error("[web:kafka] broker unreachable ({}): {} —— kafka 是显式依赖，拒绝静默运行", m_brokers,
              rd_kafka_err2str(rc));
        return -1;
    }
    rd_kafka_metadata_destroy(md);

    // 建话题（幂等）：auto-create 只在首个 produce 时生效，consumer 启动等不到——
    // 显式 CreateTopics 消除冷启动竞态（已存在 = TOPIC_ALREADY_EXISTS 视为成功）
    rd_kafka_NewTopic_t* nt = rd_kafka_NewTopic_new(m_topic.c_str(), 1, 1, errStr, sizeof(errStr));
    if (nt == nullptr) {
        Error("[web:kafka] new-topic create failed: {}", errStr);
        return -1;
    }
    rd_kafka_queue_t* q = rd_kafka_queue_new(m_rk);
    rd_kafka_CreateTopics(m_rk, &nt, 1, nullptr, q);
    rd_kafka_event_t* ev = rd_kafka_queue_poll(q, kStartTimeoutS * 1000);
    rd_kafka_resp_err_t crc = ev != nullptr ? rd_kafka_event_error(ev)
                                            : RD_KAFKA_RESP_ERR__TIMED_OUT;
    if (ev != nullptr) {
        rd_kafka_event_destroy(ev);
    }
    rd_kafka_queue_destroy(q);
    rd_kafka_NewTopic_destroy(nt);
    if (crc != RD_KAFKA_RESP_ERR_NO_ERROR &&
        crc != RD_KAFKA_RESP_ERR_TOPIC_ALREADY_EXISTS) {
        Error("[web:kafka] create topic failed: {} ({})", m_topic, rd_kafka_err2str(crc));
        return -1;
    }

    Info("[web:kafka] producer up (brokers={} topic={})", m_brokers, m_topic);
    return 0;
}

void KafkaProducer::OnDelivery(rd_kafka_t*, const rd_kafka_message_t* msg, void* ctx) {
    auto* self = static_cast<KafkaProducer*>(ctx);
    if (msg->err != RD_KAFKA_RESP_ERR_NO_ERROR) {
        self->m_failed.fetch_add(1);
        LogOnce(msg->err, "delivery");
    } else {
        self->m_delivered.fetch_add(1);
    }
}

int KafkaProducer::Produce(const uint8_t* data, uint32_t len, const std::string& key,
                           const std::vector<std::pair<std::string, std::string>>& headers) {
    if (m_rk == nullptr) {
        return -1;
    }
    rd_kafka_headers_t* hdrs = nullptr;
    if (!headers.empty()) {
        hdrs = rd_kafka_headers_new(static_cast<size_t>(headers.size()));
        for (const auto& h : headers) {
            rd_kafka_header_add(hdrs, h.first.c_str(), -1, h.second.c_str(),
                                static_cast<ssize_t>(h.second.size()));
        }
    }
    // RD_KAFKA_MSG_F_COPY：detmw 回调持有的缓冲区 produce 后即归还，拷贝入队
    rd_kafka_resp_err_t rc = rd_kafka_producev(
            m_rk, RD_KAFKA_V_RKT(m_rkt), RD_KAFKA_V_KEY(key.c_str(), key.size()),
            RD_KAFKA_V_VALUE(const_cast<uint8_t*>(data), len),
            RD_KAFKA_V_HEADERS(hdrs),  // 成功时所有权转移给 librdkafka
            RD_KAFKA_V_MSGFLAGS(RD_KAFKA_MSG_F_COPY), RD_KAFKA_V_END);
    if (rc != RD_KAFKA_RESP_ERR_NO_ERROR) {
        if (hdrs != nullptr) {
            rd_kafka_headers_destroy(hdrs);
        }
        LogOnce(rc, "produce");
        return -1;
    }
    m_queued.fetch_add(1);
    return 0;
}

void KafkaProducer::Shutdown() {
    if (m_rk != nullptr) {
        rd_kafka_flush(m_rk, kFlushTimeoutMs);
    }
    if (m_rkt != nullptr) {
        rd_kafka_topic_destroy(m_rkt);
        m_rkt = nullptr;
    }
    if (m_rk != nullptr) {
        rd_kafka_destroy(m_rk);
        m_rk = nullptr;
    }
}

// ---- KafkaConsumer ----

KafkaConsumer::KafkaConsumer(const std::string& brokers, const std::string& topic)
    : m_brokers(brokers), m_topic(topic) {}

KafkaConsumer::~KafkaConsumer() {
    Stop();
}

int KafkaConsumer::Start(MsgFn fn) {
    m_fn = std::move(fn);
    char errStr[256] = {0};
    rd_kafka_conf_t* conf = rd_kafka_conf_new();
    rd_kafka_conf_set(conf, "bootstrap.servers", m_brokers.c_str(), errStr, sizeof(errStr));
    // group.id 是高层 consumer 句柄的硬要求（无它 assign 返回 UNKNOWN_GROUP）；
    // 只 assign 不 subscribe = 不入组协议、无 rebalance；auto.commit 关 = 无位点上交
    rd_kafka_conf_set(conf, "group.id", "dts-web-ui", errStr, sizeof(errStr));
    rd_kafka_conf_set(conf, "enable.auto.commit", "false", errStr, sizeof(errStr));

    m_rk = rd_kafka_new(RD_KAFKA_CONSUMER, conf, errStr, sizeof(errStr));
    if (m_rk == nullptr) {
        rd_kafka_conf_destroy(conf);
        Error("[web:kafka] consumer create failed: {}", errStr);
        return -1;
    }
    rd_kafka_poll_set_consumer(m_rk);

    // 免组协议 assign：查 metadata 拿全分区 → 逐分 OFFSET_END（观测面 = 只看实时）
    const rd_kafka_metadata_t* md = nullptr;
    rd_kafka_resp_err_t rc = rd_kafka_metadata(m_rk, 0, nullptr, &md, kStartTimeoutS * 1000);
    if (rc != RD_KAFKA_RESP_ERR_NO_ERROR || md->topic_cnt == 0) {
        Error("[web:kafka] consumer metadata failed (topic={} 可能尚无数据写入): {}",
              m_topic, rd_kafka_err2str(rc));
        if (md != nullptr) {
            rd_kafka_metadata_destroy(md);
        }
        return -1;
    }
    const rd_kafka_metadata_topic_t* t = nullptr;
    for (int i = 0; i < md->topic_cnt; ++i) {
        if (std::strcmp(md->topics[i].topic, m_topic.c_str()) == 0) {
            t = &md->topics[i];
            break;
        }
    }
    if (t == nullptr) {
        rd_kafka_metadata_destroy(md);
        Error("[web:kafka] topic not found: {}", m_topic);
        return -1;
    }
    rd_kafka_topic_partition_list_t* parts =
            rd_kafka_topic_partition_list_new(static_cast<int>(t->partition_cnt));
    for (int i = 0; i < t->partition_cnt; ++i) {
        rd_kafka_topic_partition_t* p =
                rd_kafka_topic_partition_list_add(parts, m_topic.c_str(), t->partitions[i].id);
        p->offset = RD_KAFKA_OFFSET_END;
    }
    const int partCnt = t->partition_cnt;
    for (int i = 0; i < partCnt; ++i) {
        m_parts.push_back(t->partitions[i].id);
    }
    rd_kafka_metadata_destroy(md);

    if (rd_kafka_assign(m_rk, parts) != RD_KAFKA_RESP_ERR_NO_ERROR) {
        Error("[web:kafka] assign failed");
        rd_kafka_topic_partition_list_destroy(parts);
        return -1;
    }
    rd_kafka_topic_partition_list_destroy(parts);

    // lag 基线：启动时高水位（观测面 OFFSET_END，lag = 增量水位 - 已消费）
    for (int part : m_parts) {
        int64_t hi = 0;
        int64_t lo = 0;
        if (rd_kafka_get_watermark_offsets(m_rk, m_topic.c_str(), part, &lo, &hi) ==
            RD_KAFKA_RESP_ERR_NO_ERROR) {
            m_hiBase += hi;
        }
    }

    m_running.store(true);
    m_thread = std::thread(&KafkaConsumer::Run, this);
    Info("[web:kafka] consumer up (topic={} partitions={})", m_topic, partCnt);
    return 0;
}

void KafkaConsumer::Run() {
    int pollCnt = 0;
    while (m_running.load()) {
        rd_kafka_message_t* msg = rd_kafka_consumer_poll(m_rk, 200);
        if (msg != nullptr) {
            if (msg->err == RD_KAFKA_RESP_ERR_NO_ERROR) {
                m_consumed.fetch_add(1);
                if (m_fn) {
                    m_fn(static_cast<const uint8_t*>(msg->payload),
                         static_cast<uint32_t>(msg->len), msg->offset);
                }
            } else if (msg->err != RD_KAFKA_RESP_ERR__PARTITION_EOF &&
                       msg->err != RD_KAFKA_RESP_ERR__MSG_TIMED_OUT) {
                LogOnce(msg->err, "consume");
            }
            rd_kafka_message_destroy(msg);
        }
        if (++pollCnt % 10 == 0) {  // ~2s 刷一次 lag（broker 水位往返）
            int64_t hi = 0;
            for (int part : m_parts) {
                int64_t h = 0;
                int64_t lo = 0;
                if (rd_kafka_get_watermark_offsets(m_rk, m_topic.c_str(), part, &lo, &h) ==
                    RD_KAFKA_RESP_ERR_NO_ERROR) {
                    hi += h;
                }
            }
            m_lag.store(hi - m_hiBase - m_consumed.load());
        }
    }
}

void KafkaConsumer::Stop() {
    if (!m_running.exchange(false)) {
        return;
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
    if (m_rk != nullptr) {
        rd_kafka_consumer_close(m_rk);
        rd_kafka_destroy(m_rk);
        m_rk = nullptr;
    }
}

int64_t KafkaConsumer::lag() {
    return m_lag.load();
}

}  // namespace web
