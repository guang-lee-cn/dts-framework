#!/usr/bin/env bash
# S 级回归（真实数据链路）：1 条/s × 32K raw：spa_mock_proc（握手+上报）-> tune_dts
#   (data 线程切分 500 dataId + 合并上报) -> webserver_mock（单 reader 计数）。
# 断言端到端零丢包：web 收到的上报帧数 == spa 发送帧数。
# 用法：run_slevel.sh <dts_bin> <spa_bin> <web_bin> <gen_dir> [rounds] [interval_us] [raw_len]
#   raw_len：默认 32768（32K 真实工况，依赖 /dev/shm 或大 MTU）；受限环境
#   （无 SHM、UDP 大包不可达，如 CI 沙箱）用 DTS_RAW_LEN 环境变量调小验证链路逻辑。
set -u

DTS_BIN=$1
SPA_BIN=$2
WEB_BIN=$3
GEN_DIR=$4
ROUNDS=${5:-5}
INTERVAL_US=${6:-1000000}   # S 级：1 条/s
RAW_LEN=${7:-32768}
export DTS_RAW_LEN=$RAW_LEN
# 受限网络自适应：RTPS 报文钉在 MTU 内（RTPS 层分片，不依赖 IP 分片——WSL mirrored 等环境
# IP 分片重组不通会让多端点 SEDP 公告整批丢失/32K UDP 不可达；见 detmw_fastdds.cpp DETMW_UDP_MTU）
export DETMW_UDP_MTU=${DETMW_UDP_MTU:-1400}
# web 收集窗口：spa 发完 ROUNDS 帧 + 前后发现/排空余量
WEB_RUN_S=$((ROUNDS * INTERVAL_US / 1000000 + 8))

DTS_CFG="$GEN_DIR/tune-dts/tune-dts.json"
SPA_CFG="$GEN_DIR/tune-spa/tune-spa.json"
WEB_CFG="$GEN_DIR/tune-web/tune-web.json"

cleanup() {
  kill -9 "$DTS_PID" "$WEB_PID" 2>/dev/null
}
trap cleanup EXIT

echo "=== S-level (1/s x ${RAW_LEN}B x $ROUNDS, zero-loss) start ==="
"$DTS_BIN" "$DTS_CFG" > /tmp/slevel_dts.log 2>&1 &
DTS_PID=$!
sleep 4                              # dts 上电 + 发现

"$WEB_BIN" "$WEB_CFG" "$WEB_RUN_S" "" 1 > /tmp/slevel_web.log 2>&1 &
WEB_PID=$!
sleep 2                              # web 订阅 + 发现完成

"$SPA_BIN" "$SPA_CFG" "$ROUNDS" "$INTERVAL_US" > /tmp/slevel_spa.log 2>&1
SPA_RC=$?

wait "$WEB_PID" 2>/dev/null
kill -9 "$DTS_PID" 2>/dev/null
wait "$DTS_PID" 2>/dev/null

echo "--- spa-mock ---"; grep -E "sent|handshake" /tmp/slevel_spa.log || true
echo "--- web-mock ---"; grep -E "total_recv" /tmp/slevel_web.log || true

WEB_RECV=$(grep -oE "total_recv=[0-9]+" /tmp/slevel_web.log | grep -oE "[0-9]+" | head -1)
WEB_RECV=${WEB_RECV:-0}
echo "=== result: spa_rc=$SPA_RC web_recv=$WEB_RECV (expect $ROUNDS) ==="
# PASS：spa 发成功 + 上报帧数与发送帧数一致（零丢包）
[ "$SPA_RC" -eq 0 ] && [ "$WEB_RECV" -eq "$ROUNDS" ]
