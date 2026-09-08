#!/usr/bin/env bash
# 停止并删除 Redpanda 容器（topic 数据随容器删除，观测面无需持久化）
set -euo pipefail
podman rm -f dts-redpanda >/dev/null 2>&1 && echo "[redpanda] removed" || echo "[redpanda] not running"
