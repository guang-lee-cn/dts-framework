#!/usr/bin/env bash
# 单节点 Redpanda（Kafka API 兼容）拉起：podman 免 root；auto-create topic 免建 topic。
# 端口：9092(Kafka) / 9644(Admin API)。已存在则跳过（幂等）。
set -euo pipefail

NAME=dts-redpanda
if podman container exists "$NAME" 2>/dev/null; then
  if [ "$(podman container inspect -f '{{.State.Running}}' "$NAME" 2>/dev/null)" = "true" ]; then
    echo "[redpanda] already running (localhost:9092)"
    exit 0
  fi
  podman start "$NAME" >/dev/null
else
  echo "[redpanda] creating container..."
  podman run -d --name "$NAME" -p 9092:9092 -p 9644:9644 \
    docker.io/redpandadata/redpanda:latest \
    redpanda start --overprovisioned --smp 1 --memory 512M \
    --kafka-addr PLAINTEXT://0.0.0.0:9092 \
    --advertise-kafka-addr PLAINTEXT://localhost:9092 \
    --set redpanda.auto_create_topics_enabled=true >/dev/null
fi

# 就绪探测：admin API 端口通即认为可服务（v1 观测面足够；Kafka 端口由 web_server fail-fast 兜底）
for i in $(seq 1 20); do
  if curl -s -o /dev/null --max-time 1 http://localhost:9644/v1/status/ready; then
    echo "[redpanda] ready (kafka=localhost:9092 admin=localhost:9644)"
    exit 0
  fi
  sleep 0.5
done
echo "[redpanda] NOT ready after 10s" >&2
exit 1
