#!/usr/bin/env bash
# DDS QoS 扫描：循环不同 reliability/depth 组合，跑 tune 链路（count 模式量纯通道），
# 汇总 webserver fps/MB/s 到 CSV。
# 用法：run_qos_sweep.sh <dts_bin> <spa_bin> <web_bin> <gen_dir> <out_summary.csv> [rounds] [interval_us]
# 输出 summary CSV：reliability,depth,fps,mbps
set -u

DTS_BIN=$1
SPA_BIN=$2
WEB_BIN=$3
GEN_DIR=$4
SUMMARY=${5:-/tmp/qos_sweep.csv}
ROUNDS=${6:-20000}
INTERVAL_US=${7:-0}
RUN_S=8

RUN_TUNE="$(dirname "$0")/run_tune.sh"

echo "reliability,depth,fps,mbps" > "$SUMMARY"

for REL in reliable best_effort; do
  for DEPTH in 10 100 1000 5000; do
    echo "=== sweep reliability=$REL depth=$DEPTH ==="
    DETMW_QOS_RELIABILITY=$REL DETMW_QOS_DEPTH=$DEPTH \
      bash "$RUN_TUNE" "$DTS_BIN" "$SPA_BIN" "$WEB_BIN" "$GEN_DIR" \
           "$ROUNDS" "$INTERVAL_US" "$RUN_S" > "/tmp/qos_${REL}_d${DEPTH}.log" 2>&1

    LINE=$(grep "report=" "/tmp/qos_${REL}_d${DEPTH}.log" | tail -1)
    FPS=$(echo "$LINE" | grep -oE '[0-9.]+ fps' | grep -oE '[0-9.]+' || echo 0)
    MBPS=$(echo "$LINE" | grep -oE '[0-9.]+ MB/s' | grep -oE '[0-9.]+' || echo 0)
    echo "$REL,$DEPTH,$FPS,$MBPS" >> "$SUMMARY"
    echo "  -> fps=$FPS mbps=$MBPS"
  done
done

echo "=== QoS sweep summary -> $SUMMARY ==="
cat "$SUMMARY"
