#include <spdlog/spdlog.h>

#include <cstdarg>
#include <cstdio>

#include "detmw_log.h"

namespace {
void detmw_log(bool isError, const char* fmt, va_list args) {
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, args);
    if (isError) {
        spdlog::error("[detmw] {}", buf);
    } else {
        spdlog::info("[detmw] {}", buf);
    }
}
}  // namespace

extern "C" {

void detmw_log_info(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    detmw_log(false, fmt, args);
    va_end(args);
}

void detmw_log_error(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    detmw_log(true, fmt, args);
    va_end(args);
}

}  // extern "C"
