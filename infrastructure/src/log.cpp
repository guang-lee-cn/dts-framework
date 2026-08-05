#include "log.h"

#include <atomic>
#include <memory>
#include <vector>

#include <spdlog/async.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "ts_rotating_sink.h"

namespace dts::log {

namespace {

std::atomic<bool> g_init{false};

constexpr spdlog::level::level_enum ToSpdlogLevel(Level lvl) {
    switch (lvl) {
        case Level::TRACE:
            return spdlog::level::trace;
        case Level::DEBUG:
            return spdlog::level::debug;
        case Level::INFO:
            return spdlog::level::info;
        case Level::WARN:
            return spdlog::level::warn;
        case Level::ERROR:
            return spdlog::level::err;
        case Level::CRITICAL:
            return spdlog::level::critical;
    }
    return spdlog::level::info;
}

constexpr Level FromSpdlogLevel(spdlog::level::level_enum lvl) {
    switch (lvl) {
        case spdlog::level::trace:
            return Level::TRACE;
        case spdlog::level::debug:
            return Level::DEBUG;
        case spdlog::level::info:
            return Level::INFO;
        case spdlog::level::warn:
            return Level::WARN;
        case spdlog::level::err:
            return Level::ERROR;
        case spdlog::level::critical:
            return Level::CRITICAL;
        default:
            return Level::INFO;
    }
}

}  // namespace

int Init(const Config& cfg) {
    if (g_init.exchange(true)) {
        return 0;  // 已初始化，幂等
    }
    spdlog::init_thread_pool(cfg.poolSize, cfg.poolThreads);
    std::vector<spdlog::sink_ptr> sinks;
    if (cfg.console) {
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    }
    if (cfg.file.enable) {
        sinks.push_back(std::make_shared<TsRotatingSink>(cfg.file.dir, cfg.file.namePattern,
                                                         cfg.file.maxSizeMb, cfg.file.maxTotalMb));
    }
    auto logger = std::make_shared<spdlog::async_logger>(
        "dts", sinks.begin(), sinks.end(), spdlog::thread_pool(),
        spdlog::async_overflow_policy::overrun_oldest);
    spdlog::set_default_logger(logger);
    spdlog::set_level(ToSpdlogLevel(cfg.level));
    return 0;
}

void Shutdown() {
    spdlog::shutdown();
    g_init.store(false);
}

void SetLevel(Level lvl) {
    spdlog::set_level(ToSpdlogLevel(lvl));
}

Level GetLevel() {
    return FromSpdlogLevel(spdlog::get_level());
}

}  // namespace dts::log
