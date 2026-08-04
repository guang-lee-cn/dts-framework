#include "detmw.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "detmw_fastdds.h"
#include "detmw_log.h"

#define DETMW_MAX_TOPICS 64
#define DETMW_STR_LEN 32
#define DETMW_TOPIC_LEN 128

#define ROLE_SUBSCRIBE 0
#define ROLE_PUBLISH 1

// 配置端点：进程生成 JSON（gen_detmw.py 注入 entity_id/user_id）逐条解析
struct detmw_topic_entry {
    char sessionType[DETMW_STR_LEN];
    char sessionInst[DETMW_STR_LEN];
    char thread[DETMW_STR_LEN];
    uint32_t msgId;
    int role;  // ROLE_SUBSCRIBE / ROLE_PUBLISH
    uint16_t userId;
    uint16_t entityId;
    char topic[DETMW_TOPIC_LEN];
};

struct detmw_handle {
    int domain_id;
    char process[DETMW_STR_LEN];
    void* transport;
    struct detmw_topic_entry topics[DETMW_MAX_TOPICS];
    int topicCount;
};

// 通信标识规则：sessionType + sessionInst + msgId -> DDS topic 名（订阅/发布同规则）
static void MakeTopic(char* out, size_t cap, const char* sessionType, const char* sessionInst,
                      uint32_t msgId) {
    snprintf(out, cap, "%s_%s_%u", sessionType, sessionInst, msgId);
}

static struct detmw_topic_entry* FindTopic(detmw_handle* h, const char* sessionType,
                                           const char* sessionInst, uint32_t msgId, int role) {
    for (int i = 0; i < h->topicCount; i++) {
        struct detmw_topic_entry* e = &h->topics[i];
        if (e->msgId == msgId && e->role == role &&
            strcmp(e->sessionType, sessionType) == 0 &&
            strcmp(e->sessionInst, sessionInst) == 0) {
            return e;
        }
    }
    return NULL;
}

// 从生成 JSON 加载配置：domain / process / topics（role + thread + 端点 ID）
static int LoadConfig(detmw_handle* h, const char* cfg_path) {
    FILE* fp = fopen(cfg_path, "rb");
    if (fp == NULL) {
        detmw_log_error("open config failed: %s", cfg_path);
        return -1;
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size <= 0 || size > 1 << 20) {
        fclose(fp);
        detmw_log_error("config size invalid: %s", cfg_path);
        return -1;
    }
    char* buf = (char*)malloc((size_t)size + 1);
    if (buf == NULL) {
        fclose(fp);
        return -1;
    }
    size_t rd = fread(buf, 1, (size_t)size, fp);
    fclose(fp);
    buf[rd] = '\0';

    cJSON* root = cJSON_Parse(buf);
    free(buf);
    if (root == NULL) {
        detmw_log_error("config parse failed: %s", cfg_path);
        return -1;
    }

    const char* process = cJSON_GetStringValue(cJSON_GetObjectItem(root, "process"));
    cJSON* domain = cJSON_GetObjectItem(root, "domain");
    cJSON* topics = cJSON_GetObjectItem(root, "topics");
    if (process == NULL || !cJSON_IsNumber(domain) || !cJSON_IsArray(topics)) {
        detmw_log_error("config missing process/domain/topics: %s", cfg_path);
        cJSON_Delete(root);
        return -1;
    }

    snprintf(h->process, sizeof(h->process), "%s", process);
    h->domain_id = (int)domain->valuedouble;

    int n = cJSON_GetArraySize(topics);
    if (n > DETMW_MAX_TOPICS) n = DETMW_MAX_TOPICS;
    for (int i = 0; i < n; i++) {
        cJSON* t = cJSON_GetArrayItem(topics, i);
        if (t == NULL) continue;
        const char* st = cJSON_GetStringValue(cJSON_GetObjectItem(t, "session_type"));
        const char* si = cJSON_GetStringValue(cJSON_GetObjectItem(t, "session_inst"));
        cJSON* mid = cJSON_GetObjectItem(t, "msg_id");
        const char* role = cJSON_GetStringValue(cJSON_GetObjectItem(t, "role"));
        const char* thread = cJSON_GetStringValue(cJSON_GetObjectItem(t, "thread"));
        cJSON* eid = cJSON_GetObjectItem(t, "entity_id");
        cJSON* uid = cJSON_GetObjectItem(t, "user_id");
        if (st == NULL || si == NULL || !cJSON_IsNumber(mid) || role == NULL) {
            detmw_log_error("topic[%d] invalid in config", i);
            continue;
        }
        struct detmw_topic_entry* e = &h->topics[h->topicCount++];
        strncpy(e->sessionType, st, DETMW_STR_LEN - 1);
        strncpy(e->sessionInst, si, DETMW_STR_LEN - 1);
        e->sessionType[DETMW_STR_LEN - 1] = '\0';
        e->sessionInst[DETMW_STR_LEN - 1] = '\0';
        if (thread != NULL) {
            strncpy(e->thread, thread, DETMW_STR_LEN - 1);
            e->thread[DETMW_STR_LEN - 1] = '\0';
        } else {
            e->thread[0] = '\0';
        }
        e->msgId = (uint32_t)mid->valuedouble;
        e->role = (strcmp(role, "publish") == 0) ? ROLE_PUBLISH : ROLE_SUBSCRIBE;
        e->userId = cJSON_IsNumber(uid) ? (uint16_t)uid->valuedouble : 0;
        e->entityId = cJSON_IsNumber(eid) ? (uint16_t)eid->valuedouble : 0;
        MakeTopic(e->topic, sizeof(e->topic), st, si, e->msgId);
    }

    cJSON_Delete(root);
    if (h->topicCount == 0) {
        detmw_log_error("no topics loaded from config");
        return -1;
    }
    return 0;
}

// 生成配置同目录的组静态发现 XML（gen_detmw.py 每进程目录各写一份）
static int MakeStaticXmlPath(char* out, size_t cap, const char* cfg_path) {
    const char* slash = strrchr(cfg_path, '/');
    if (slash == NULL) {
        snprintf(out, cap, "staticdiscovery.xml");
        return 0;
    }
    size_t dir_len = (size_t)(slash - cfg_path) + 1;  // 含尾部 '/'
    if (dir_len + (size_t)sizeof("staticdiscovery.xml") > cap) {
        return -1;
    }
    memcpy(out, cfg_path, dir_len);
    snprintf(out + dir_len, cap - dir_len, "staticdiscovery.xml");
    return 0;
}

detmw_handle* detmw_init(const char* cfg_path) {
    if (cfg_path == NULL) {
        detmw_log_error("cfg_path is NULL");
        return NULL;
    }
    detmw_handle* h = (detmw_handle*)calloc(1, sizeof(detmw_handle));
    if (h == NULL) return NULL;
    h->domain_id = -1;

    if (LoadConfig(h, cfg_path) != 0) {
        free(h);
        return NULL;
    }

    char static_xml[DETMW_TOPIC_LEN];
    if (MakeStaticXmlPath(static_xml, sizeof(static_xml), cfg_path) != 0) {
        detmw_log_error("staticdiscovery.xml path too long");
        free(h);
        return NULL;
    }

    h->transport = detmw_fastdds_create(h->domain_id, h->process, static_xml);
    if (h->transport == NULL) {
        detmw_log_error("transport create failed (domain=%d)", h->domain_id);
        free(h);
        return NULL;
    }

    // 预建发布端 writer：静态发现端点尽早注册（ESW 位图置位），首包不丢
    for (int i = 0; i < h->topicCount; i++) {
        struct detmw_topic_entry* e = &h->topics[i];
        if (e->role == ROLE_PUBLISH) {
            if (detmw_fastdds_create_writer(h->transport, e->topic, e->userId, e->entityId) != 0) {
                detmw_log_error("writer precreate failed: %s", e->topic);
            }
        }
    }

    detmw_log_info("init ok (process=%s domain=%d topics=%d)", h->process, h->domain_id,
                   h->topicCount);
    return h;
}

void detmw_destroy(detmw_handle* h) {
    if (h == NULL) return;
    if (h->transport != NULL) {
        detmw_fastdds_destroy(h->transport);
    }
    free(h);
}

int detmw_subscribe(detmw_handle* h, const char* sessionType, const char* sessionInst,
                    uint32_t msgId, detmw_recv_fn fn, void* userCtx) {
    struct detmw_topic_entry* e = FindTopic(h, sessionType, sessionInst, msgId, ROLE_SUBSCRIBE);
    if (e == NULL) {
        detmw_log_error("subscribe topic not in config: %s_%s_%u", sessionType, sessionInst, msgId);
        return -1;
    }
    return detmw_fastdds_subscribe(h->transport, e->topic, e->userId, e->entityId, fn, userCtx);
}

int detmw_publish(detmw_handle* h, const char* sessionType, const char* sessionInst,
                  uint32_t msgId, const uint8_t* data, uint32_t len) {
    struct detmw_topic_entry* e = FindTopic(h, sessionType, sessionInst, msgId, ROLE_PUBLISH);
    if (e == NULL) {
        detmw_log_error("publish topic not in config: %s_%s_%u", sessionType, sessionInst, msgId);
        return -1;
    }
    return detmw_fastdds_publish(h->transport, e->topic, data, len);
}

int detmw_dump(detmw_handle* h, char* buf, size_t cap) {
    size_t off = 0;
    int n = snprintf(buf, cap, "[detmw] process=%s domain=%d topics=%d\n", h->process, h->domain_id,
                     h->topicCount);
    if (n > 0) off = (size_t)n;
    for (int i = 0; i < h->topicCount && off < cap; i++) {
        struct detmw_topic_entry* e = &h->topics[i];
        n = snprintf(buf + off, cap - off, "  %s %s.%s.%u role=%s uid=%u eid=%u\n", e->topic,
                     e->sessionType, e->sessionInst, e->msgId,
                     e->role == ROLE_PUBLISH ? "pub" : "sub", e->userId, e->entityId);
        if (n > 0) off += (size_t)n;
    }
    return 0;
}
