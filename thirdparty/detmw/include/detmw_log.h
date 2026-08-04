#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// detmw C 部分日志桥：包装 spdlog（C++ 实现，C ABI 给 detmw.c 调用）
void detmw_log_info(const char* fmt, ...);
void detmw_log_error(const char* fmt, ...);

#ifdef __cplusplus
}
#endif
