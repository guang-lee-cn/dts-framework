#!/usr/bin/env python3
"""dts 控制面 CLI：登录 dts 进程 console 的交互式会话 / 单条命令 / 发现实例。

用法：
    dts-cli.py ps                      # 发现运行实例（/tmp/dts-*.sock）
    dts-cli.py login <name>            # 交互式会话（提示符 '# '，quit 退出）
    dts-cli.py exec <name> <cmd...>    # 单条命令（如 exec cpf-dts get_threads）

协议：AF_UNIX 长连接，一行一条命令，响应以 \\x00 结尾。
"""

import argparse
import glob
import os
import socket
import sys

SOCK_DIR = "/tmp"


def sock_path(name: str) -> str:
    return os.path.join(SOCK_DIR, f"dts-{name}.sock")


def connect(name: str) -> socket.socket:
    path = sock_path(name)
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        s.connect(path)
    except OSError as e:
        sys.exit(f"[dts] connect {path} failed: {e}（进程未运行？）")
    return s


def recv_until(s: socket.socket) -> str:
    """读响应直到 \\x00（一条命令一条响应）。"""
    buf = b""
    while b"\x00" not in buf:
        data = s.recv(4096)
        if not data:
            break
        buf += data
    return buf[:-1].decode(errors="replace") if buf.endswith(b"\x00") else buf.decode(errors="replace")


def cmd_ps(_args) -> None:
    found = False
    for p in sorted(glob.glob(os.path.join(SOCK_DIR, "dts-*.sock"))):
        name = os.path.basename(p)[len("dts-"):-len(".sock")]
        print(f"{name:12} {p}")
        found = True
    if not found:
        print("[dts] no running dts instance")


def cmd_login(args) -> None:
    s = connect(args.name)
    print(f"# dts console · {args.name}（quit / Ctrl-D 退出）")
    try:
        while True:
            try:
                line = input("# ").strip()
            except EOFError:
                break
            if not line:
                continue
            if line in ("quit", "exit"):
                break
            s.sendall((line + "\n").encode())
            print(recv_until(s))
    finally:
        s.close()


def cmd_exec(args) -> None:
    if not args.command:
        sys.exit("[dts] exec 需要命令，如: dts-cli.py exec cpf-dts get_threads")
    s = connect(args.name)
    try:
        s.sendall((" ".join(args.command) + "\n").encode())
        print(recv_until(s))
    finally:
        s.close()


def main() -> None:
    parser = argparse.ArgumentParser(description="dts 控制面 CLI")
    sub = parser.add_subparsers(dest="sub", required=True)

    p_ps = sub.add_parser("ps", help="发现运行实例")
    p_ps.set_defaults(fn=cmd_ps)

    p_login = sub.add_parser("login", help="交互式会话")
    p_login.add_argument("name")
    p_login.set_defaults(fn=cmd_login)

    p_exec = sub.add_parser("exec", help="单条命令")
    p_exec.add_argument("name")
    p_exec.add_argument("command", nargs=argparse.REMAINDER)
    p_exec.set_defaults(fn=cmd_exec)

    args = parser.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
