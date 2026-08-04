#pragma once
// AUTO-GENERATED from sched_defs.json by tools/gen_sched_defs.py. DO NOT EDIT.

#include <cstddef>
#include <sched.h>

namespace detsched {

// 系统保留（不分配，留给内核/平台 RT 线程）
constexpr int SYS_RESERVE_START = 90;
constexpr int SYS_RESERVE_END   = 99;

// 线程优先级：跨进程统一，按业务域分段
struct SchedPrio {
    enum Core : int {
        CORE_PRIO_START = 75,
        CORE_PRIO_END   = 89,
    };
    enum Data : int {
        DATA_PRIO_START = 60,
        DATA_PRIO_END   = 74,
    };
    enum App : int {
        APP_PRIO_START = 45,
        APP_PRIO_END   = 59,
    };
    enum Dts : int {
        DTS_PRIO_START = 32,
        DTS_PRIO_END   = 34,
        DATA_PRIO = 34,
        TASK_PRIO = 33,
        LOG_PRIO = 32,
    };
    enum Bkg : int {
        BKG_PRIO_START = 15,
        BKG_PRIO_END   = 29,
    };
    enum Def : int {
        DEF_PRIO_START = 1,
        DEF_PRIO_END   = 14,
    };
};

constexpr size_t SCHED_DOMAIN_COUNT = 6;

struct SchedSegment {
    int minPrio;
    int maxPrio;
    int schedPolicy;
    const char* name;
};

constexpr SchedSegment KSCHED_SEGMENTS[SCHED_DOMAIN_COUNT] = {
    {SchedPrio::Core::CORE_PRIO_START, SchedPrio::Core::CORE_PRIO_END, SCHED_FIFO, "CORE"},
    {SchedPrio::Data::DATA_PRIO_START, SchedPrio::Data::DATA_PRIO_END, SCHED_FIFO, "DATA"},
    {SchedPrio::App::APP_PRIO_START, SchedPrio::App::APP_PRIO_END, SCHED_RR, "APP"},
    {SchedPrio::Dts::DTS_PRIO_START, SchedPrio::Dts::DTS_PRIO_END, SCHED_RR, "DTS"},
    {SchedPrio::Bkg::BKG_PRIO_START, SchedPrio::Bkg::BKG_PRIO_END, SCHED_OTHER, "BKG"},
    {SchedPrio::Def::DEF_PRIO_START, SchedPrio::Def::DEF_PRIO_END, SCHED_OTHER, "DEF"},
};

inline int SegmentIndex(int prio) {
    for (size_t i = 0; i < SCHED_DOMAIN_COUNT; ++i) {
        if (prio >= KSCHED_SEGMENTS[i].minPrio && prio <= KSCHED_SEGMENTS[i].maxPrio) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

inline const SchedSegment* FindSegment(int prio) {
    int idx = SegmentIndex(prio);
    return idx >= 0 ? &KSCHED_SEGMENTS[idx] : nullptr;
}

}  // namespace detsched
