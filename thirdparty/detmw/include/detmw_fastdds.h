#pragma once

#include <stdint.h>

#include "detmw.h"

#ifdef __cplusplus
extern "C" {
#endif

// detmw 内部：Fast-DDS 传输适配（C++ 实现，C ABI 桥）
// 静态发现：participant 建 EDP=STATIC，加载 <staticdiscovery> XML（组端点目录），
//   端点按配置显式设 entity_id / user_defined_id（须与 XML 一致）。
void* detmw_fastdds_create(int domain_id, const char* process_name, const char* static_xml_path);
int   detmw_fastdds_subscribe(void* h, const char* topic,
                              uint16_t user_defined_id, uint16_t entity_id,
                              detmw_recv_fn fn, void* ctx);
// 预建发布端 writer（静态发现端点尽早注册；发布走既有 writer）
int   detmw_fastdds_create_writer(void* h, const char* topic,
                                  uint16_t user_defined_id, uint16_t entity_id);
int   detmw_fastdds_publish(void* h, const char* topic, const uint8_t* data, uint32_t len);
void  detmw_fastdds_destroy(void* h);

#ifdef __cplusplus
}
#endif
