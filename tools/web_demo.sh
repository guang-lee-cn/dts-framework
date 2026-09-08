#!/usr/bin/env bash
# web-server 演示：一条命令拉起完整观测链路，浏览器打开 http://localhost:8080
#   链路：spa_mock_proc（持续 1 帧/s raw）→ tune_dts（切分+合并上报）→ web_server
#         → Kafka(dts.report 沉淀) → consumer → SSE → 浏览器实时观测
# 用法：tools/web_demo.sh [rounds]   # rounds=0 表示持续发送（默认 0），Ctrl+C 全链清理
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)
BUILD=$ROOT/build
GEN=$BUILD/generated/detmw-tune

bash tools/redpanda_up.sh
bash quality/test/clean_residue.sh force >/dev/null 2>&1 || true
pkill -9 -x web_server 2>/dev/null || true
export DETMW_UDP_MTU=${DETMW_UDP_MTU:-1400}
ROUNDS=${1:-0}

cd "$BUILD/webserver"
"$BUILD/webserver/web_server" "$GEN/tune-web2/tune-web2.json" localhost:9092 8080 dts.report ./web \
  > /tmp/demo_web.log 2>&1 &
WEB=$!
cd "$ROOT"
sleep 2
"$BUILD/quality/tune_dts" "$GEN/tune-dts/tune-dts.json" > /tmp/demo_dts.log 2>&1 &
DTS=$!
sleep 4
DTS_RAW_LEN=${DTS_RAW_LEN:-1024} "$BUILD/quality/spa_mock_proc" "$GEN/tune-spa/tune-spa.json" \
  "$ROUNDS" 1000000 > /tmp/demo_spa.log 2>&1 &
SPA=$!

cleanup() {
  echo; echo "[demo] shutting down..."
  kill -INT $SPA $WEB $DTS 2>/dev/null || true
  sleep 1
  kill -9 $SPA $WEB $DTS 2>/dev/null || true
}
trap cleanup INT TERM

echo "[demo] 浏览器打开: http://localhost:8080  (Ctrl+C 结束)"
echo "[demo] kafka: podman exec dts-redpanda rpk topic consume dts.report -n 5"
wait $SPA 2>/dev/null || true
if [ "$ROUNDS" != "0" ]; then
  sleep 3; cleanup
fi
