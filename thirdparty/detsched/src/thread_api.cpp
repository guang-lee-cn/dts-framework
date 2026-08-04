#include "thread_api.h"

#include "thread_factory.h"

namespace detsched {

bool DeclareDomain(int prioLevel) {
    return ThreadFactory::Instance().DeclareDomain(prioLevel);
}

ThreadHandle CreateThread(const std::string& name, int prioLevel, ThreadFn fn, void* arg) {
    return ThreadFactory::Instance().CreateThread(name, prioLevel, fn, arg, 0, -1, nullptr);
}

ThreadHandle CreateThread(const std::string& name, int prioLevel, const ThreadParams& params) {
    return ThreadFactory::Instance().CreateThread(name, prioLevel, nullptr, nullptr, 0, -1,
                                                  &params);
}

void DestroyThread(ThreadHandle h) {
    ThreadFactory::Instance().DestroyThread(h);
}

size_t QueryThreads(ThreadInfo* out, size_t cap) {
    return ThreadFactory::Instance().QueryThreads(out, cap);
}

void DumpThreadInfo() {
    ThreadFactory::Instance().DumpThreadInfo();
}

}  // namespace detsched
