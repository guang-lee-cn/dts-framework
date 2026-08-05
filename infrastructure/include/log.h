#pragma once

#include <cstddef>
#include <string>
#include <utility>

#include <spdlog/spdlog.h>

namespace dts::log {

// 日志级别（契约 §5）
enum class Level { TRACE, DEBUG, INFO, WARN, ERROR, CRITICAL };

struct Config {
    size_t poolSize = 8192;   // 异步队列条数
    size_t poolThreads = 1;   // 后台消费线程数
    Level level = Level::INFO;
    bool console = true;      // 终端输出
    struct File {
        bool enable = true;                  // 文件输出（TsRotatingSink）
        std::string dir = "/var/log/dts";    // 日志目录
        std::string namePattern = "dts_{}-%Y%m%d%H%M%S";  // {} = 线程名，其余 strftime 展开
        size_t maxSizeMb = 5;                // 单文件上限，超限切新时间戳文件
        size_t maxTotalMb = 5120;            // 总量上限（满态删一增一）
    } file;
};

// 初始化：async logger + stdout sink。幂等：重复调用仅首次生效
int Init(const Config& cfg = {});
void Shutdown();             // 停异步线程池，刷空队列
void SetLevel(Level lvl);
Level GetLevel();

// 通用入口（Level 参数化）与分级便捷函数：转发 spdlog，调用方零感知底层
template <typename... Args>
inline void Log(Level lvl, spdlog::format_string_t<Args...> fmt, Args&&... args) {
    switch (lvl) {
        case Level::TRACE:
            spdlog::trace(fmt, std::forward<Args>(args)...);
            break;
        case Level::DEBUG:
            spdlog::debug(fmt, std::forward<Args>(args)...);
            break;
        case Level::INFO:
            spdlog::info(fmt, std::forward<Args>(args)...);
            break;
        case Level::WARN:
            spdlog::warn(fmt, std::forward<Args>(args)...);
            break;
        case Level::ERROR:
            spdlog::error(fmt, std::forward<Args>(args)...);
            break;
        case Level::CRITICAL:
            spdlog::critical(fmt, std::forward<Args>(args)...);
            break;
    }
}

template <typename... Args>
inline void Trace(spdlog::format_string_t<Args...> fmt, Args&&... args) {
    Log(Level::TRACE, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void Debug(spdlog::format_string_t<Args...> fmt, Args&&... args) {
    Log(Level::DEBUG, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void Info(spdlog::format_string_t<Args...> fmt, Args&&... args) {
    Log(Level::INFO, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void Warn(spdlog::format_string_t<Args...> fmt, Args&&... args) {
    Log(Level::WARN, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void Error(spdlog::format_string_t<Args...> fmt, Args&&... args) {
    Log(Level::ERROR, fmt, std::forward<Args>(args)...);
}

}  // namespace dts::log
