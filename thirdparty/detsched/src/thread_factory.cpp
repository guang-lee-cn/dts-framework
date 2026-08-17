#include "thread_factory.h"

#include "log.h"

#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>   // getenv：DTS_STRICT_SCHED
#include <cstring>
#include <string>
#include <utility>

namespace detsched {

namespace {
constexpr int THREAD_STATE_IDLE = 0;
constexpr int THREAD_STATE_RUNNING = 1;
}  // namespace

struct ThreadHandleImpl {
    std::string m_name;
    int m_segIndex;
    int m_prio;
    int m_schedPolicy;
    int m_cpuAffinity;
    bool m_detached;
    ThreadFn m_fn;
    void* m_fnArg;
    pthread_t m_tid;
    std::atomic<bool> m_stop{false};
    int m_state = THREAD_STATE_IDLE;
};

ThreadFactory& ThreadFactory::Instance() {
    static ThreadFactory inst;
    return inst;
}

bool ThreadFactory::DeclareDomain(int prioLevel) {
    int segIdx = SegmentIndex(prioLevel);
    if (segIdx < 0) {
        dts::log::Error("[detsched] DeclareDomain: prio {} no domain", prioLevel);
        return false;
    }
    m_domains[segIdx] = true;
    dts::log::Info("[detsched] domain {} declared (prio={})", KSCHED_SEGMENTS[segIdx].name,
                prioLevel);
    return true;
}

void* ThreadFactory::Entry(void* arg) {
    auto* h = static_cast<ThreadHandleImpl*>(arg);
    if (h->m_fn != nullptr) {
        h->m_fn(h->m_fnArg);
    } else {
        while (!h->m_stop.load()) {
            usleep(100 * 1000);
        }
    }
    return nullptr;
}

ThreadHandle ThreadFactory::CreateThread(const std::string& name, int prioLevel,
                                         ThreadFn fn, void* fnArg,
                                         size_t stackBytes, int cpuAffinity,
                                         const ThreadParams* params) {
    int segIdx = SegmentIndex(prioLevel);
    if (segIdx < 0) {
        dts::log::Error("[detsched] {}: prio {} no domain", name.c_str(), prioLevel);
        return nullptr;
    }
    if (!m_domains[segIdx]) {
        dts::log::Error("[detsched] {}: domain {} not declared", name.c_str(),
                     KSCHED_SEGMENTS[segIdx].name);
        return nullptr;
    }
    const SchedSegment& seg = KSCHED_SEGMENTS[segIdx];

    int prio = params && params->schedPriority >= 0 ? params->schedPriority : prioLevel;
    int policy = params && params->schedPolicy >= 0 ? params->schedPolicy : seg.schedPolicy;
    int affinity = params ? params->cpuAffinity : cpuAffinity;
    size_t stack = params ? params->stackSize : stackBytes;
    ThreadFn entry = params ? params->fn : fn;
    void* entryArg = params ? params->arg : fnArg;
    if (prio < seg.minPrio || prio > seg.maxPrio) {
        dts::log::Error("[detsched] {}: prio {} out of [{},{}] for domain {}", name.c_str(),
                     prio, seg.minPrio, seg.maxPrio, seg.name);
        return nullptr;
    }

    // 重复线程名告警（不拒绝）
    {
        std::lock_guard<std::mutex> lk(m_regMutex);
        for (const auto* it : m_registry) {
            if (it->m_name == name) {
                dts::log::Error("[detsched] {}: duplicate thread name", name.c_str());
            }
        }
    }

    auto* h = new ThreadHandleImpl();
    h->m_name = name;
    h->m_segIndex = segIdx;
    h->m_prio = prio;
    h->m_schedPolicy = policy;
    h->m_cpuAffinity = affinity;
    h->m_detached = params && params->detached;
    h->m_fn = entry;
    h->m_fnArg = entryArg;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (stack > 0) {
        pthread_attr_setstacksize(&attr, stack);
    }
    if (policy != SCHED_OTHER) {
        sched_param param{};
        param.sched_priority = prio;
        pthread_attr_setschedpolicy(&attr, policy);
        pthread_attr_setschedparam(&attr, &param);
        pthread_attr_setinheritsched(&attr,
                                     (params && !params->explicitSched) ? PTHREAD_INHERIT_SCHED
                                                                        : PTHREAD_EXPLICIT_SCHED);
    }
    if (h->m_detached) {
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    }
    if (params && params->guardSize > 0) {
        pthread_attr_setguardsize(&attr, params->guardSize);
    }

    int rc = pthread_create(&h->m_tid, &attr, Entry, h);
    if (rc == EPERM && policy != SCHED_OTHER) {
        // RT 权限不足（容器/非 root 无 CAP_SYS_NICE）。
        // 默认降级普通调度（dev/test 可跑，生产有 RT 权限时仍走 RT，确定性语义不变）；
        // DTS_STRICT_SCHED=1 严格模式：RT 不可用 = 部署/配置错误 → 拒绝启动（商用 fail-fast，
        // 无确定性保证时不许静默运行）
        if (std::getenv("DTS_STRICT_SCHED") != nullptr) {
            dts::log::Error("[detsched] {}: RT create failed ({}) and DTS_STRICT_SCHED=1: "
                            "refusing to run without real-time scheduling (fail-fast)",
                            name.c_str(), std::strerror(rc));
            pthread_attr_destroy(&attr);
            delete h;
            return nullptr;
        }
        dts::log::Warn("[detsched] {}: RT create failed ({}), fallback to SCHED_OTHER",
                     name.c_str(), std::strerror(rc));
        pthread_attr_destroy(&attr);
        pthread_attr_init(&attr);
        if (stack > 0) {
            pthread_attr_setstacksize(&attr, stack);
        }
        if (h->m_detached) {
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        }
        if (params && params->guardSize > 0) {
            pthread_attr_setguardsize(&attr, params->guardSize);
        }
        h->m_schedPolicy = SCHED_OTHER;
        h->m_prio = 0;  // SCHED_OTHER 无静态优先级
        rc = pthread_create(&h->m_tid, &attr, Entry, h);
    }
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        dts::log::Error("[detsched] {}: pthread_create failed ({})", name.c_str(),
                     std::strerror(rc));
        delete h;
        return nullptr;
    }

    pthread_setname_np(h->m_tid, name.c_str());

    if (affinity >= 0) {
        long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
        if (affinity >= ncpu) {
            dts::log::Error("[detsched] {}: affinity {} out of range (ncpu={})",
                         name.c_str(), affinity, ncpu);
        } else {
            cpu_set_t set;
            CPU_ZERO(&set);
            CPU_SET(affinity, &set);
            if (pthread_setaffinity_np(h->m_tid, sizeof(set), &set) != 0) {
                dts::log::Error("[detsched] {}: setaffinity({}) failed", name.c_str(),
                             affinity);
            }
        }
    }

    h->m_state = THREAD_STATE_RUNNING;

    {
        std::lock_guard<std::mutex> lk(m_regMutex);
        m_registry.push_back(h);
    }

    dts::log::Info("[detsched] {} created (domain={} prio={} policy={} cpu={} detached={})",
                name.c_str(), seg.name, prio, policy, affinity, h->m_detached);
    return h;
}

void ThreadFactory::DestroyThread(ThreadHandle h) {
    if (h == nullptr) return;
    {
        std::lock_guard<std::mutex> lk(m_regMutex);
        auto it = std::find(m_registry.begin(), m_registry.end(), h);
        if (it != m_registry.end()) {
            m_registry.erase(it);
        }
    }
    h->m_stop.store(true);
    if (!h->m_detached) {
        pthread_join(h->m_tid, nullptr);
    }
    h->m_state = THREAD_STATE_IDLE;
    delete h;
}

size_t ThreadFactory::QueryThreads(ThreadInfo* out, size_t cap) {
    std::lock_guard<std::mutex> lk(m_regMutex);
    size_t n = std::min(cap, m_registry.size());
    for (size_t i = 0; i < n; ++i) {
        const auto* h = m_registry[i];
        out[i].name = h->m_name;
        out[i].segIndex = h->m_segIndex;
        out[i].prio = h->m_prio;
        out[i].schedPolicy = h->m_schedPolicy;
        out[i].cpuAffinity = h->m_cpuAffinity;
        out[i].tid = static_cast<uint64_t>(h->m_tid);
        out[i].state = h->m_state;
    }
    return n;
}

void ThreadFactory::DumpThreadInfo() {
    std::lock_guard<std::mutex> lk(m_regMutex);
    dts::log::Info("[detsched] --- thread info ({}) ---", m_registry.size());
    for (const auto* h : m_registry) {
        dts::log::Info("[detsched] %-16s domain=%-6s prio=%3d policy={} cpu=%3d tid={}",
                    h->m_name.c_str(), KSCHED_SEGMENTS[h->m_segIndex].name, h->m_prio, h->m_schedPolicy,
                    h->m_cpuAffinity, static_cast<unsigned long>(h->m_tid));
    }
    dts::log::Info("[detsched] -----------------------");
}

}  // namespace detsched
