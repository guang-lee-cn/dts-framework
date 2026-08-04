#!/usr/bin/env python3
"""生成 platform_sched_defs.h：读 sched_defs.json，构建期静态校验 + 生成。

用法: gen_sched_defs.py <sched_defs.json> <output.h>
校验失败即退出非 0，阻断构建。
"""
import json
import re
import sys

POLICIES = {"SCHED_FIFO", "SCHED_RR", "SCHED_OTHER"}
UPPER = re.compile(r"[A-Z][A-Z0-9_]*")


def fail(msg):
    print(f"[gen] error: {msg}", file=sys.stderr)
    return False


def validate(cfg):
    ok = True

    reserve = cfg.get("system_reserve", {})
    rs, re_ = reserve.get("start"), reserve.get("end")
    if rs is None or re_ is None:
        ok = fail("system_reserve requires start/end")
    elif rs > re_:
        ok = fail(f"system_reserve start {rs} > end {re_}")

    domains = cfg.get("domains", [])
    if not domains:
        ok = fail("domains empty")

    seen_names = set()
    seen_roles = set()
    prev = None  # (start, end) 前一域区间
    for d in domains:
        name = d.get("name")
        start, end = d.get("start"), d.get("end")
        policy = d.get("policy")
        if not UPPER.match(name or ""):
            ok = fail(f"domain {name}: name must be UPPER_SNAKE")
        if name in seen_names:
            ok = fail(f"domain {name}: duplicated")
        seen_names.add(name)
        if not isinstance(start, int) or not isinstance(end, int):
            ok = fail(f"domain {name}: start/end must be int")
        elif start > end:
            ok = fail(f"domain {name}: start {start} > end {end}")
        if policy not in POLICIES:
            ok = fail(f"domain {name}: invalid policy {policy}")
        if prev is not None:
            # 区间相交判定：NOT (end < prev_start OR prev_end < start)
            if not (end < prev[0] or prev[1] < start):
                ok = fail(f"domain {name}: interval [{start},{end}] overlaps previous [{prev[0]},{prev[1]}]")
        prev = (start, end)

        for role, prio in d.get("roles", {}).items():
            if not UPPER.match(role):
                ok = fail(f"domain {name}: role {role} must be UPPER_SNAKE")
            if role in seen_roles:
                ok = fail(f"domain {name}: role {role} duplicated globally (enum value leak)")
            seen_roles.add(role)
            if not isinstance(prio, int) or not (start <= prio <= end):
                ok = fail(f"domain {name}: role {role} prio {prio} out of [{start},{end}]")
    return ok


def render(cfg):
    reserve = cfg["system_reserve"]
    domains = cfg["domains"]
    L = []

    L.append("#pragma once")
    L.append("// AUTO-GENERATED from sched_defs.json by tools/gen_sched_defs.py. DO NOT EDIT.")
    L.append("")
    L.append("#include <cstddef>")
    L.append("#include <sched.h>")
    L.append("")
    L.append("namespace detsched {")
    L.append("")
    L.append(f"// 系统保留（不分配，留给内核/平台 RT 线程）")
    L.append(f"constexpr int SYS_RESERVE_START = {reserve['start']};")
    L.append(f"constexpr int SYS_RESERVE_END   = {reserve['end']};")
    L.append("")
    L.append("// 线程优先级：跨进程统一，按业务域分段")
    L.append("struct SchedPrio {")
    for d in domains:
        name, start, end = d["name"], d["start"], d["end"]
        camel = name.title()
        L.append(f"    enum {camel} : int {{")
        L.append(f"        {name}_PRIO_START = {start},")
        L.append(f"        {name}_PRIO_END   = {end},")
        for role, prio in d.get("roles", {}).items():
            L.append(f"        {role} = {prio},")
        L.append("    };")
    L.append("};")
    L.append("")
    L.append(f"constexpr size_t SCHED_DOMAIN_COUNT = {len(domains)};")
    L.append("")
    L.append("struct SchedSegment {")
    L.append("    int minPrio;")
    L.append("    int maxPrio;")
    L.append("    int schedPolicy;")
    L.append("    const char* name;")
    L.append("};")
    L.append("")
    L.append(f"constexpr SchedSegment KSCHED_SEGMENTS[SCHED_DOMAIN_COUNT] = {{")
    for d in domains:
        name, policy = d["name"], d["policy"]
        camel = name.title()
        L.append(f"    {{SchedPrio::{camel}::{name}_PRIO_START, SchedPrio::{camel}::{name}_PRIO_END, "
                 f"{policy}, \"{name}\"}},")
    L.append("};")
    L.append("")
    L.append("inline int SegmentIndex(int prio) {")
    L.append("    for (size_t i = 0; i < SCHED_DOMAIN_COUNT; ++i) {")
    L.append("        if (prio >= KSCHED_SEGMENTS[i].minPrio && prio <= KSCHED_SEGMENTS[i].maxPrio) {")
    L.append("            return static_cast<int>(i);")
    L.append("        }")
    L.append("    }")
    L.append("    return -1;")
    L.append("}")
    L.append("")
    L.append("inline const SchedSegment* FindSegment(int prio) {")
    L.append("    int idx = SegmentIndex(prio);")
    L.append("    return idx >= 0 ? &KSCHED_SEGMENTS[idx] : nullptr;")
    L.append("}")
    L.append("")
    L.append("}  // namespace detsched")
    L.append("")
    return "\n".join(L)


def main():
    if len(sys.argv) != 3:
        print("usage: gen_sched_defs.py <sched_defs.json> <output.h>", file=sys.stderr)
        return 1
    cfg_path, out_path = sys.argv[1], sys.argv[2]
    try:
        with open(cfg_path) as f:
            cfg = json.load(f)
    except (OSError, ValueError) as e:
        print(f"[gen] error: load {cfg_path}: {e}", file=sys.stderr)
        return 1
    if not validate(cfg):
        return 1
    with open(out_path, "w") as f:
        f.write(render(cfg))
    print(f"[gen] wrote {out_path} ({len(cfg['domains'])} domains)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
