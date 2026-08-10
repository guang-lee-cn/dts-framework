#!/usr/bin/env bash
# DDS 调优链路编排：tune_dts + spa_mock_proc（握手+上报rawData）+ webserver_mock（收上报落CSV）
# 用法：run_tune.sh <dts_bin> <spa_bin> <web_bin> <gen_dir> [rounds] [interval_us] [run_s] [out_csv]
#   out_csv 给了 → web 落 CSV（量 CSV 极限）；不给 → web 只计数（量纯通道极限）
set -u

DTS_BIN=$1
SPA_BIN=$2
WEB_BIN=$3
GEN_DIR=$4
ROUNDS=${5:-1000}
INTERVAL_US=${6:-0}
RUN_S=${7:-8}
OUT_CSV=${8:-}

DTS_CFG="$GEN_DIR/tune-dts/tune-dts.json"
SPA_CFG="$GEN_DIR/tune-spa/tune-spa.json"
WEB_CFG="$GEN_DIR/tune-web/tune-web.json"

# 内存守护：MEM_LIMIT_MB（默认 2G）监控三进程 RSS，超限 kill 全部 + 报警（防系统卡死/断连）
MEM_LIMIT_MB=${MEM_LIMIT_MB:-2048}
# CPU 绑核：测试进程只用 TUNE_CPUS（默认 0-7，8 核），留核给 IDE/系统，防满载拖死 vscode
TUNE_CPUS=${TUNE_CPUS:-0-7}
TUNE_PIDS=""
guard_mem() {
  while true; do
    local sum=0 rss
    for pid in $TUNE_PIDS; do
      rss=$(ps -o rss= -p "$pid" 2>/dev/null | awk '{print $1}')
      sum=$((sum + ${rss:-0}))
    done
    local mbs=$((sum / 1024))
    if [ -n "$TUNE_PIDS" ] && [ "$mbs" -gt "$MEM_LIMIT_MB" ]; then
      echo "[mem-guard] RSS ${mbs}MB > limit ${MEM_LIMIT_MB}MB, killing all tune processes"
      pkill -9 -x tune_dts 2>/dev/null
      pkill -9 -x spa_mock_proc 2>/dev/null
      pkill -9 -x webserver_mock 2>/dev/null
      exit 2
    fi
    sleep 0.5
  done
}
cleanup() {
  kill -9 "$DTS_PID" "$WEB_PID" 2>/dev/null
  pkill -9 -x tune_dts 2>/dev/null
  pkill -9 -x spa_mock_proc 2>/dev/null
  pkill -9 -x webserver_mock 2>/dev/null
}
trap cleanup EXIT

echo "=== DDS tune chain start (rounds=$ROUNDS interval=${INTERVAL_US}us run=${RUN_S}s csv=${OUT_CSV:-none} mem_limit=${MEM_LIMIT_MB}MB cpus=${TUNE_CPUS}) ==="
taskset -c "$TUNE_CPUS" "$DTS_BIN" "$DTS_CFG" > /tmp/tune_dts.log 2>&1 &
DTS_PID=$!
sleep 4                              # dts 上电 + 静态发现

if [ -n "$OUT_CSV" ]; then
    taskset -c "$TUNE_CPUS" "$WEB_BIN" "$WEB_CFG" "$RUN_S" "$OUT_CSV" > /tmp/tune_web.log 2>&1 &
else
    taskset -c "$TUNE_CPUS" "$WEB_BIN" "$WEB_CFG" "$RUN_S" > /tmp/tune_web.log 2>&1 &
fi
WEB_PID=$!
sleep 2                              # web 订阅 + 发现完成

TUNE_PIDS="$DTS_PID $WEB_PID"
guard_mem &
GUARD_PID=$!

taskset -c "$TUNE_CPUS" "$SPA_BIN" "$SPA_CFG" "$ROUNDS" "$INTERVAL_US" > /tmp/tune_spa.log 2>&1
SPA_RC=$?

kill "$GUARD_PID" 2>/dev/null        # spa 发完，停 guard

wait "$WEB_PID" 2>/dev/null          # web 收 RUN_S 后自行退出（析构 segfault 忽略）
kill -INT "$DTS_PID" 2>/dev/null
sleep 1
kill -9 "$DTS_PID" 2>/dev/null

echo "--- spa-mock ---"; grep -E "sent|handshake" /tmp/tune_spa.log || true
echo "--- web-mock ---"; grep -E "report=" /tmp/tune_web.log || true
echo "--- dts data recv (reader[3]=msg3 raw) ---"; grep "reader\[3\]" /tmp/tune_dts.log || true

WEB_FPS=$(grep "report=" /tmp/tune_web.log | grep -oE '[0-9.]+ fps' | grep -oE '[0-9.]+' | tail -1)
WEB_FPS=${WEB_FPS:-0}
echo "=== result: spa_rc=$SPA_RC web_fps=$WEB_FPS ==="
# PASS：spa 发成功 + web 收到上报（fps > 0）
[ "$SPA_RC" -eq 0 ] && awk "BEGIN{exit !($WEB_FPS > 0)}"
