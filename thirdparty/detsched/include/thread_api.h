#pragma once

#include <cstdint>
#include <string>

#include "platform_sched_defs.h"

namespace detsched {

// 线程入口：业务函数
using ThreadFn = void (*)(void* arg);

struct ThreadParams {
    ThreadFn fn = nullptr;         // 线程入口
    void* arg = nullptr;
    size_t stackSize = 0;          // 0 = 系统默认
    int    schedPolicy = -1;       // -1 = 跟随业务域
    int    schedPriority = -1;     // -1 = 使用档位值
    bool   explicitSched = true;
    int    cpuAffinity = -1;       // -1 = 不绑
    bool   detached = false;
    size_t guardSize = 0;
};

struct ThreadInfo {
    std::string name;
    int segIndex;
    int prio;
    int schedPolicy;
    int cpuAffinity;
    uint64_t tid;
    int state;
};

struct ThreadHandleImpl;
using ThreadHandle = ThreadHandleImpl*;

bool DeclareDomain(int prioLevel);

// 主入口：线程入口默认空；栈/亲和等复杂参数用结构体版
ThreadHandle CreateThread(const std::string& name, int prioLevel,
                          ThreadFn fn = nullptr, void* arg = nullptr);

ThreadHandle CreateThread(const std::string& name, int prioLevel,
                          const ThreadParams& params);

void DestroyThread(ThreadHandle h);

size_t QueryThreads(ThreadInfo* out, size_t cap);
void   DumpThreadInfo();

}  // namespace detsched
