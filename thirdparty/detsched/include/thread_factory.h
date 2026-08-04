#pragma once

#include <mutex>
#include <vector>

#include "thread_api.h"

// 通用惯用法：禁止拷贝 + 移动（放类私有区末尾）
#define DETSCHED_NONCOPYABLE(Class)          \
    Class(const Class&) = delete;            \
    Class& operator=(const Class&) = delete; \
    Class(Class&&) = delete;                 \
    Class& operator=(Class&&) = delete;

namespace detsched {

class ThreadFactory {
public:
    static ThreadFactory& Instance();

    bool DeclareDomain(int prioLevel);
    ThreadHandle CreateThread(const std::string& name, int prioLevel,
                              ThreadFn fn, void* arg,
                              size_t stackBytes, int cpuAffinity,
                              const ThreadParams* params);
    void DestroyThread(ThreadHandle h);
    size_t QueryThreads(ThreadInfo* out, size_t cap);
    void DumpThreadInfo();

private:
    ThreadFactory() = default;
    ~ThreadFactory() = default;
    DETSCHED_NONCOPYABLE(ThreadFactory)

    static void* Entry(void* arg);

    bool m_domains[SCHED_DOMAIN_COUNT]{};
    std::mutex m_regMutex;
    std::vector<ThreadHandle> m_registry;
};

}  // namespace detsched
