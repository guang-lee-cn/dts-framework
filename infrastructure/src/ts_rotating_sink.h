#pragma once

#include <cstdio>
#include <mutex>
#include <string>

#include <spdlog/sinks/base_sink.h>

namespace dts::log {

// 文件三件事（契约 §5 / D9）：时间戳文件名 + 单文件大小切分 + 目录总量删旧。
// 锁由 base_sink 负责（多后台线程共写时互斥）。
class TsRotatingSink : public spdlog::sinks::base_sink<std::mutex> {
public:
    TsRotatingSink(std::string dir, std::string name_pattern, size_t max_size_mb, size_t max_total_mb);
    ~TsRotatingSink() override;

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override;
    void flush_() override;

private:
    // 关闭旧文件，打开新时间戳文件（目录不存在则建）
    void OpenNewFile();
    // 目录内本前缀文件总量超限 → 删最旧 1 个
    void EvictOldestIfNeeded();

    std::string m_dir;
    std::string m_namePrefix;   // namePattern 去掉时间戳部分，用于识别/删旧
    std::string m_tsFmt;        // strftime 时间戳格式（namePattern 的 % 段）
    size_t m_maxSize;           // 字节
    size_t m_maxTotal;          // 字节
    std::string m_currentPath;
    std::FILE* m_file = nullptr;
    size_t m_bytes = 0;
    bool m_openFailed = false;  // 目录不可写：首次 stderr 提示后静默
};

}  // namespace dts::log
