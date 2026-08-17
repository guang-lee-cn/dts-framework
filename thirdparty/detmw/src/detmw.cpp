#include "detmw.h"
#include "detmw_transport.h"

#include "log.h"

#include <cJSON.h>

#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace detmw {

namespace {

// 本地订阅条目：回调 + 上下文（bootstrap 的 Subscription*，OnRouteMsg 投 mailbox）
struct LocalSub {
    recv_fn fn = nullptr;
    void* ctx = nullptr;
};

// 配置端点：进程生成 JSON（gen_detmw.py 注入 entity_id/user_id）逐条解析
struct ConfigEntry {
    endpoint ep;
    bool is_publish = false;  // false = subscribe
    uint32_t plain_size = 0;  // >0 = 定长 plain 通道（严格等长，DataSharing 零拷贝候选）
    std::string topic;        // MakeTopic(sessionType, sessionInst, msgId)
};

// 通信标识规则：endpoint -> DDS topic 名（订阅/发布同规则，由 gen 配置固化）
std::string MakeTopic(const char* st, const char* si, uint32_t mid) {
    return std::string(st) + "_" + si + "_" + std::to_string(mid);
}

std::string MakeStaticXmlPath(const char* cfg_path) {
    const char* slash = strrchr(cfg_path, '/');
    if (slash == nullptr) return "staticdiscovery.xml";
    return std::string(cfg_path, slash - cfg_path + 1) + "staticdiscovery.xml";
}

}  // namespace

// Communicator 内部：配置表 + 传输实现 + 本进程订阅登记（publish_internal 直通用）
struct Communicator::Impl {
    std::vector<ConfigEntry> config;                // 配置表（加载于构造）
    std::unique_ptr<TransportInterface> transport;  // 底层传输（FastDDS 实现）
    int domain_id = -1;
    std::string process;
    std::unordered_map<std::string, uint32_t> plainTopics;  // topic → 定长 plain（0 = BytesType）

    // 本进程订阅表：subscribe() 登记，publish_internal() 命中 → mailbox 直投（免序列化）
    // key = endpoint（sessionType.sessionInst.msgId），value = 该端点全部本地订阅回调
    std::unordered_map<endpoint, std::vector<LocalSub>, endpoint_hash> localSubs;
    mutable std::mutex subMutex;  // 保护 localSubs（订阅在建线程启动前，发布在业务线程）
};

Communicator::Communicator(const char* cfg_path) : m_impl(new Impl()) {
    if (cfg_path == nullptr) {
        dts::log::Error("[detmw] cfg_path is null");
        return;
    }

    // 1. 解析配置 JSON：process / domain / topics
    FILE* fp = fopen(cfg_path, "rb");
    if (fp == nullptr) {
        dts::log::Error("[detmw] open config failed: {}", cfg_path);
        return;
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size <= 0 || size > 1 << 20) {
        fclose(fp);
        dts::log::Error("[detmw] config size invalid: {}", cfg_path);
        return;
    }
    std::string buf(static_cast<size_t>(size), '\0');
    size_t rd = fread(buf.data(), 1, static_cast<size_t>(size), fp);
    fclose(fp);
    buf.resize(rd);

    cJSON* root = cJSON_Parse(buf.c_str());
    if (root == nullptr) {
        dts::log::Error("[detmw] config parse failed: {}", cfg_path);
        return;
    }

    const char* process = cJSON_GetStringValue(cJSON_GetObjectItem(root, "process"));
    cJSON* domain = cJSON_GetObjectItem(root, "domain");
    cJSON* topics = cJSON_GetObjectItem(root, "topics");
    if (process == nullptr || !cJSON_IsNumber(domain) || !cJSON_IsArray(topics)) {
        dts::log::Error("[detmw] config missing process/domain/topics: {}", cfg_path);
        cJSON_Delete(root);
        return;
    }

    m_impl->process = process;
    m_impl->domain_id = static_cast<int>(domain->valuedouble);

    int n = cJSON_GetArraySize(topics);
    for (int i = 0; i < n; i++) {
        cJSON* t = cJSON_GetArrayItem(topics, i);
        if (t == nullptr) continue;
        const char* st = cJSON_GetStringValue(cJSON_GetObjectItem(t, "session_type"));
        const char* si = cJSON_GetStringValue(cJSON_GetObjectItem(t, "session_inst"));
        cJSON* mid = cJSON_GetObjectItem(t, "msg_id");
        const char* role = cJSON_GetStringValue(cJSON_GetObjectItem(t, "role"));
        if (st == nullptr || si == nullptr || !cJSON_IsNumber(mid) || role == nullptr) {
            dts::log::Error("[detmw] topic[{}] invalid in config", i);
            continue;
        }
        ConfigEntry e;
        e.ep.session_type = st;
        e.ep.session_inst = si;
        e.ep.msg_id = static_cast<uint32_t>(mid->valuedouble);
        e.is_publish = (std::string(role) == "publish");
        e.topic = MakeTopic(st, si, e.ep.msg_id);
        // 可选 plain_size：>0 = 定长 plain 通道（严格等长，DataSharing 零拷贝候选）
        cJSON* ps = cJSON_GetObjectItem(t, "plain_size");
        if (cJSON_IsNumber(ps) && ps->valuedouble > 0) {
            e.plain_size = static_cast<uint32_t>(ps->valuedouble);
            m_impl->plainTopics[e.topic] = e.plain_size;
            dts::log::Info("[detmw] topic {} plain_size={} (fixed-size zero-copy channel)",
                           e.topic, e.plain_size);
        }
        m_impl->config.push_back(std::move(e));
    }
    cJSON_Delete(root);

    if (m_impl->config.empty()) {
        dts::log::Error("[detmw] no topics loaded from config");
        return;
    }

    // 2. 创建传输（FastDDS participant + 动态/静态发现；plain 类型表随构造传入）
    const std::string static_xml = MakeStaticXmlPath(cfg_path);
    m_impl->transport = CreateFastDdsTransport(m_impl->domain_id, m_impl->process.c_str(),
                                               static_xml.c_str(), m_impl->plainTopics);

    // 3. 预建发布端 writer：静态发现端点尽早注册，首包不丢
    if (m_impl->transport) {
        for (const auto& e : m_impl->config) {
            if (e.is_publish) {
                m_impl->transport->CreateWriter(e.ep);
            }
        }
        dts::log::Info("[detmw] init ok (process={} domain={} topics={})", m_impl->process,
                     m_impl->domain_id, m_impl->config.size());
    }
}

Communicator::~Communicator() {
    delete m_impl;
}

bool Communicator::good() const {
    return m_impl != nullptr && m_impl->transport != nullptr;
}

int Communicator::subscribe(const endpoint& src, recv_fn fn, void* ctx) {
    if (!m_impl->transport) {
        dts::log::Error("[detmw] subscribe: transport not ready");
        return -1;
    }
    const int rc = m_impl->transport->CreateReader(src, fn, ctx);
    if (rc != 0) {
        return rc;
    }
    // 登记本地订阅：publish_internal 命中后 mailbox 直投（免 DDS 序列化）
    std::lock_guard<std::mutex> lk(m_impl->subMutex);
    m_impl->localSubs[src].push_back(LocalSub{fn, ctx});
    return 0;
}

int Communicator::publish_external(const endpoint& dst, const uint8_t* data, uint32_t len) {
    if (!m_impl->transport) {
        dts::log::Error("[detmw] publish_external: transport not ready");
        return -1;
    }
    return m_impl->transport->Send(dst, data, len);
}

int Communicator::publish_internal(const endpoint& dst, const uint8_t* data, uint32_t len) {
    // 进程内直通（D7/D8）：命中本进程订阅者 → 直接调订阅回调（OnRouteMsg → mailbox.Send），
    // 免 DDS 序列化/传输/反序列化；接收侧仍是统一路由（契约 detmw.md §3）。
    // 未命中 → 回退 transport（对端可能在别的进程，走 DDS）。
    {
        std::lock_guard<std::mutex> lk(m_impl->subMutex);
        auto it = m_impl->localSubs.find(dst);
        if (it != m_impl->localSubs.end() && !it->second.empty()) {
            // 每个订阅者独立 payload（回调消费 unique_ptr 所有权；多订阅者各自拷贝）
            for (const LocalSub& s : it->second) {
                if (s.fn == nullptr) {
                    continue;
                }
                auto payload = std::make_unique<std::vector<uint8_t>>();
                if (data != nullptr && len > 0) {
                    payload->assign(data, data + len);
                }
                s.fn(s.ctx, std::move(payload));
            }
            return 0;
        }
    }
    if (!m_impl->transport) {
        dts::log::Error("[detmw] publish_internal: transport not ready");
        return -1;
    }
    return m_impl->transport->Send(dst, data, len);
}

int Communicator::dump(char* buf, size_t cap) const {
    if (buf == nullptr || cap == 0) return -1;
    size_t off = 0;
    int n = snprintf(buf, cap, "[detmw] process=%s domain=%d topics=%zu\n",
                     m_impl->process.c_str(), m_impl->domain_id, m_impl->config.size());
    if (n > 0) off = static_cast<size_t>(n);
    for (const auto& e : m_impl->config) {
        n = snprintf(buf + off, cap - off, "  %s %s role=%s\n", e.topic.c_str(),
                     e.ep.ToString().c_str(), e.is_publish ? "pub" : "sub");
        if (n > 0) off += static_cast<size_t>(n);
    }
    return 0;
}

}  // namespace detmw
