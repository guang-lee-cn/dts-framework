#include <cstdio>
#include <unistd.h>

#include "thread_api.h"

using namespace detsched;

namespace {
void collectWork(void* arg) {
    int* count = static_cast<int*>(arg);
    std::printf("[demo] collect thread running, arg=%d\n", *count);
}

void idleThread(void*) {
    std::printf("[demo] idle thread running\n");
}
}  // namespace

int main() {
    DeclareDomain(SchedPrio::Dts::DATA_PRIO);
    DeclareDomain(SchedPrio::Def::DEF_PRIO_START);

    // 主入口：fn + arg（4 参数）
    int cnt = 7;
    auto* t1 = CreateThread("dts_data", SchedPrio::Dts::DATA_PRIO, collectWork, &cnt);
    // 无入口：默认空，占位挂起
    auto* t2 = CreateThread("def_idle", SchedPrio::Def::DEF_PRIO_START, idleThread, nullptr);
    // 结构体版：栈 + CPU 亲和 + detached
    ThreadParams p{};
    p.fn = collectWork;
    p.arg = &cnt;
    p.stackSize = 256 * 1024;
    p.cpuAffinity = 0;
    p.detached = true;
    auto* t3 = CreateThread("def_det", SchedPrio::Def::DEF_PRIO_START, p);

    auto* bad = CreateThread("bad_core", SchedPrio::Core::CORE_PRIO_START);
    std::printf("[demo] bad_core handle = %p (expect nullptr)\n", static_cast<void*>(bad));

    usleep(200 * 1000);
    DumpThreadInfo();

    DestroyThread(t1);
    DestroyThread(t2);
    DestroyThread(t3);
    return 0;
}
