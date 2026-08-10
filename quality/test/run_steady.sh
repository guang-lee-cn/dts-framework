#!/usr/bin/env bash
# 稳态测量：每轮调 run_tune（全新三进程，discovery 干净），spa rounds 设大值跑满 web 收集时间，
# 量稳态 web fps。多轮取中位数（消除单次波动）。
# 用法：run_steady.sh <dts_bin> <spa_bin> <web_bin> <gen_dir> [run_s] [rounds_n] [interval_us]
#   run_s=web 收集秒；rounds_n=轮数；interval_us=spa 间隔
set -u

DTS_BIN=$1
SPA_BIN=$2
WEB_BIN=$3
GEN_DIR=$4
RUN_S=${5:-8}
ROUNDS_N=${6:-3}
INTERVAL_US=${7:-0}

RUN_TUNE="$(dirname "$0")/run_tune.sh"

# spa 全速 ~10000 msg/s，rounds 给足让它跑满 RUN_S（×1.5 保险）
SPA_ROUNDS=$((RUN_S * 15000))

FPS_LIST=()
for r in $(seq 1 "$ROUNDS_N"); do
  bash "$RUN_TUNE" "$DTS_BIN" "$SPA_BIN" "$WEB_BIN" "$GEN_DIR" "$SPA_ROUNDS" "$INTERVAL_US" "$RUN_S" \
    > "/tmp/steady_${r}.log" 2>&1
  FPS=$(grep "result:" "/tmp/steady_${r}.log" | grep -oE 'web_fps=[0-9.]+' | grep -oE '[0-9.]+')
  SPA_MS=$(grep "sent" "/tmp/steady_${r}.log" | grep -oE '[0-9.]+ msg/s' | grep -oE '[0-9.]+' | tail -1)
  MBPS=$(grep "sent" "/tmp/steady_${r}.log" | grep -oE '[0-9.]+ MB/s' | grep -oE '[0-9.]+' | tail -1)
  FPS=${FPS:-0}; SPA_MS=${SPA_MS:-0}; MBPS=${MBPS:-0}
  echo "  round $r: web_fps=$FPS  spa=${SPA_MS}msg/s ${MBPS}MB/s"
  FPS_LIST+=("$FPS")
done

# 中位数
MEDIAN=$(printf '%s\n' "${FPS_LIST[@]}" | sort -n | awk -v n="$ROUNDS_N" 'NR==int((n+1)/2)')
echo "=== steady result: rounds=$ROUNDS_N web_fps median=$MEDIAN (values: ${FPS_LIST[*]}) ==="
