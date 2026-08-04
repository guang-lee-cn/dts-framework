#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// detmw —— 统一进程间通信中间件（C ABI 稳定，底层传输可插拔）
// 配置驱动：detmw_init 加载进程生成配置 JSON（gen_detmw.py 产物），
//   含 domain / process / topics（session+msgId+role+thread+端点ID），
//   传输层据此建 participant（EDP=STATIC 静态发现）+ 各端点显式 entityId。
// 通信标识规则：DDS topic = sessionType_sessionInst_msgId（确定性映射）。

typedef struct detmw_handle detmw_handle;

// 统一接收回调：对齐 itran (session+msgId) → fn(data,len)
typedef void (*detmw_recv_fn)(void* user_ctx, const uint8_t* data, uint32_t len);

// 生命周期：加载进程生成配置，建 participant（静态发现）+ 预建发布端 writer
detmw_handle* detmw_init(const char* cfg_path);
void detmw_destroy(detmw_handle* h);

// 订阅：按 (sessionType+sessionInst+msgId) 查配置 subscribe 端点并绑定回调
int detmw_subscribe(detmw_handle* h,
                    const char* session_type, const char* session_inst, uint32_t msg_id,
                    detmw_recv_fn fn, void* user_ctx);

// 发布：按 (sessionType+sessionInst+msgId) 查配置 publish 端点（writer 已预建）
int detmw_publish(detmw_handle* h,
                  const char* session_type, const char* session_inst, uint32_t msg_id,
                  const uint8_t* data, uint32_t len);

// 调试：dump 配置端点
int detmw_dump(detmw_handle* h, char* buf, size_t cap);

#ifdef __cplusplus
}
#endif
