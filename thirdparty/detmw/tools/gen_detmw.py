#!/usr/bin/env python3
"""生成 detmw 配置组产物：线程路由头 + 进程生成配置 + 组静态发现 XML。

用法: gen_detmw.py <config_dir> <output_dir>
扫描 <config_dir> 下所有 <proc>.json（进程配置），产出：
  <out>/<proc>/{task,data,log}_routes.h    线程路由头（Sub/Pub 分表，代码 include）
  <out>/<proc>/<proc>.json                 进程生成配置（注入端点 entity_id/user_id，detmw 运行时加载）
  <out>/<proc>/staticdiscovery.xml         组静态发现端点目录（每进程目录各一份，内容一致）
配置源（json）提交 + 评审，生成物构建期生成，不提交。
"""
import json
import os
import sys

INSTS = ("task", "data", "log")
TOPIC_LEN = 128


def load_processes(cfg_dir):
    procs = []
    for root, _, files in os.walk(cfg_dir):
        for fn in sorted(files):
            if not fn.endswith(".json"):
                continue
            with open(os.path.join(root, fn)) as f:
                cfg = json.load(f)
            if "process" not in cfg:
                print(f"[gen] skip {fn}: no process field", file=sys.stderr)
                continue
            procs.append(cfg)
    return procs


def topic_name(session_type, session_inst, msg_id):
    return f"{session_type}_{session_inst}_{msg_id}"


def gen_routes_h(proc, inst, routes, out_path):
    """routes: [(session_type, session_inst, msg_id, role)]"""
    name = inst.title()
    L = ["// AUTO-GENERATED from config/detmw/dts by gen_detmw.py. DO NOT EDIT.",
         "#pragma once",
         "",
         "#include <cstdint>",
         "#include <cstddef>",
         "",
         "namespace dts {",
         "",
         f"// {inst} 线程路由（{proc}）：Sub=订阅（外部 -> 线程），Pub=发布（线程 -> 外部）",
         f"struct {name}Route {{",
         "    const char* sessionType;",
         "    const char* sessionInst;",
         "    uint32_t msgId;",
         "};"]
    # Sub/Pub 分表恒定生成（可能为空），startup 无条件引用
    for kind, role in (("Sub", "subscribe"), ("Pub", "publish")):
        rs = [r for r in routes if r[3] == role]
        L.append(f"inline constexpr {name}Route k{name}{kind}Routes[] = {{")
        for session_type, session_inst, msg_id, _ in rs:
            L.append(f'    {{"{session_type}", "{session_inst}", {msg_id}}},')
        L.append("};")
        L.append(f"inline constexpr size_t k{name}{kind}RouteCount = "
                 f"sizeof(k{name}{kind}Routes) / sizeof(k{name}{kind}Routes[0]);")
    L += ["", "}  // namespace dts", ""]
    with open(out_path, "w") as f:
        f.write("\n".join(L) + "\n")
    print(f"[gen] wrote {out_path}")


def gen_static_xml(procs, out_path):
    L = ['<?xml version="1.0" encoding="UTF-8" ?>',
         "<staticdiscovery>"]
    for p in procs:
        L.append("    <participant>")
        L.append(f"        <name>{p['process']}</name>")
        for t in p["topics"]:
            role = t.get("role", "subscribe")
            tag = "writer" if role == "publish" else "reader"
            L.append(f"        <{tag}>")
            L.append(f"            <entityID>{t['entity_id']}</entityID>")
            L.append(f"            <userId>{t['user_id']}</userId>")
            L.append(f"            <topicName>{topic_name(t['session_type'], t['session_inst'], t['msg_id'])}</topicName>")
            L.append("            <topicDataType>detmw::Bytes</topicDataType>")
            L.append("            <topicKind>NO_KEY</topicKind>")
            L.append("            <reliabilityQos>RELIABLE_RELIABILITY_QOS</reliabilityQos>")
            L.append(f"        </{tag}>")
        L.append("    </participant>")
    L.append("</staticdiscovery>")
    with open(out_path, "w") as f:
        f.write("\n".join(L) + "\n")
    print(f"[gen] wrote {out_path}")


def main():
    if len(sys.argv) != 3:
        print("usage: gen_detmw.py <config_dir> <output_dir>", file=sys.stderr)
        return 1

    cfg_dir, out_dir = sys.argv[1], sys.argv[2]
    procs = load_processes(cfg_dir)
    if not procs:
        print(f"[gen] no process configs found under {cfg_dir}", file=sys.stderr)
        return 1

    # 全局端点编号：进程按名排序，端点按 config 顺序，entityID=userId=全局序号（XML 解析器要求全局唯一）
    seq = 0
    for p in sorted(procs, key=lambda x: x["process"]):
        for t in p["topics"]:
            seq += 1
            t["entity_id"] = seq
            t["user_id"] = seq

    for p in procs:
        proc = p["process"]
        odir = os.path.join(out_dir, proc)
        os.makedirs(odir, exist_ok=True)

        # 线程路由头：按 thread 分组，Sub/Pub 分表。
        # 三个线程头**恒定生成**（无路由 = 空表）：startup 无条件 include 全部三个
        # （run.cpp 引 task/data/log_routes.h），缺头会让纯单线程进程编不过
        for inst in INSTS:
            routes = [(t.get("session_type"), t.get("session_inst"), t.get("msg_id"), t.get("role"))
                      for t in p["topics"] if t.get("thread") == inst and t.get("msg_id") is not None]
            gen_routes_h(proc, inst, routes, os.path.join(odir, f"{inst}_routes.h"))

        # 进程生成配置：源配置 + 端点 ID（detmw 运行时加载）
        out_json = os.path.join(odir, f"{proc}.json")
        with open(out_json, "w") as f:
            json.dump(p, f, indent=4)
        print(f"[gen] wrote {out_json}")

        # 组静态发现 XML（每进程目录各一份，内容一致）
        gen_static_xml(procs, os.path.join(odir, "staticdiscovery.xml"))

    return 0


if __name__ == "__main__":
    sys.exit(main())
