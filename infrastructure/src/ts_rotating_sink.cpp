#include "ts_rotating_sink.h"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <vector>

namespace dts::log {

namespace {
constexpr const char* kDefaultTsFmt = "%Y%m%d%H%M%S";
constexpr const char* kThreadName = "dts";  // 当前单 default logger；每线程 logger 落地后替换为线程名
}  // namespace

TsRotatingSink::TsRotatingSink(std::string dir, std::string name_pattern, size_t max_size_mb,
                               size_t max_total_mb)
    : m_dir(std::move(dir)),
      m_maxSize(max_size_mb * 1024 * 1024),
      m_maxTotal(max_total_mb * 1024 * 1024) {
    // 拆 pattern：第一个 '%' 前为文件名前缀（{} 换线程名），其后为 strftime 时间戳
    std::string ts_fmt = kDefaultTsFmt;
    const size_t pct = name_pattern.find('%');
    if (pct != std::string::npos) {
        m_namePrefix = name_pattern.substr(0, pct);
        ts_fmt = name_pattern.substr(pct);
    } else {
        m_namePrefix = name_pattern;
    }
    const size_t br = m_namePrefix.find("{}");
    if (br != std::string::npos) {
        m_namePrefix.replace(br, 2, kThreadName);
    }
    m_tsFmt = ts_fmt;
}

TsRotatingSink::~TsRotatingSink() {
    if (m_file) {
        std::fclose(m_file);
        m_file = nullptr;
    }
}

void TsRotatingSink::sink_it_(const spdlog::details::log_msg& msg) {
    if (!m_file) {
        OpenNewFile();
        if (!m_file) {
            return;  // 目录不可写：stderr 已提示，静默丢弃
        }
    }
    spdlog::memory_buf_t formatted;
    formatter_->format(msg, formatted);
    const size_t n = std::fwrite(formatted.data(), 1, formatted.size(), m_file);
    m_bytes += n;
    std::fflush(m_file);  // 文件日志实时落盘（进程异常退出不丢）
    if (m_bytes >= m_maxSize) {
        OpenNewFile();          // 超单文件上限 → 切新时间戳文件
        EvictOldestIfNeeded();  // 总量超限 → 删最旧
    }
}

void TsRotatingSink::flush_() {
    if (m_file) {
        std::fflush(m_file);
    }
}

void TsRotatingSink::OpenNewFile() {
    if (m_openFailed) {
        return;  // 目录已确认不可写，静默降级
    }
    if (m_file) {
        std::fclose(m_file);
        m_file = nullptr;
    }
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(m_dir, ec);
    if (ec) {
        std::fprintf(stderr, "[dts:log] create dir %s failed: %s\n", m_dir.c_str(), ec.message().c_str());
        m_openFailed = true;
        return;
    }
    char ts[64];
    std::time_t t = std::time(nullptr);
    std::tm local{};
    localtime_r(&t, &local);
    if (std::strftime(ts, sizeof(ts), m_tsFmt.c_str(), &local) == 0) {
        ts[0] = '\0';
    }
    // 同秒连续切分：时间戳相同会 append 回同一文件 → 追加序号消歧（ts、ts-1、ts-2）
    int seq = 0;
    std::string path;
    do {
        path = m_dir + "/" + m_namePrefix + ts;
        if (seq > 0) {
            path += "-" + std::to_string(seq);
        }
        path += ".log";
        seq++;
    } while (path == m_currentPath && seq < 1000);
    m_currentPath = std::move(path);
    m_file = std::fopen(m_currentPath.c_str(), "ab");
    m_bytes = 0;
    if (!m_file) {
        std::fprintf(stderr, "[dts:log] open %s failed\n", m_currentPath.c_str());
        m_openFailed = true;
        m_currentPath.clear();
    }
}

void TsRotatingSink::EvictOldestIfNeeded() {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<fs::path> files;
    uintmax_t total = 0;
    for (auto& entry : fs::directory_iterator(m_dir, ec)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string fn = entry.path().filename().string();
        const bool hasExt = fn.size() >= 4 && fn.compare(fn.size() - 4, 4, ".log") == 0;
        if (fn.rfind(m_namePrefix, 0) != 0 || !hasExt) {
            continue;
        }
        files.push_back(entry.path());
        total += entry.file_size();  // 总量含当前文件（磁盘真实占用）
    }
    // 满态删一增一：总量超限 → 删 mtime 最旧 1 个（跳过当前打开的文件）
    while (total > m_maxTotal) {
        // 候选 = 除当前文件外的历史文件
        auto oldest = std::min_element(
            files.begin(), files.end(),
            [this](const fs::path& a, const fs::path& b) {
                const bool aCur = !m_currentPath.empty() && a == m_currentPath;
                const bool bCur = !m_currentPath.empty() && b == m_currentPath;
                if (aCur != bCur) {
                    return bCur;  // 当前文件排最后（不选它）
                }
                return fs::last_write_time(a) < fs::last_write_time(b);
            });
        if (oldest == files.end()) {
            break;
        }
        if (!m_currentPath.empty() && *oldest == m_currentPath) {
            break;  // 只剩当前文件，不再删
        }
        const uintmax_t sz = fs::file_size(*oldest, ec);
        if (fs::remove(*oldest, ec)) {
            total -= sz;
        }
        files.erase(oldest);
    }
}

}  // namespace dts::log
