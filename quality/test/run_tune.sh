#!/usr/bin/env bash
# DDS 调优链路编排：tune_dts + spa_mock_proc（握手+上报rawData）+ webserver_mock（收上报落CSV）
# 用法：run_tune.sh <dts_bin> <spa_bin> <web_bin> <gen_dir> [rounds] [interval_us] [run_s] [out_csv]
set -u

DTS_BIN=$1
SPA_BIN=$2
WEB_BIN=$3
GEN_DIR=$4
ROUNDS=${5:-1000}
INTERVAL_US=${6:-1000}
RUN_S=${7:-8}
OUT_CSV=${8:-/tmp/tune_report.csv}

DTS_CFG="$GEN_DIR/tune-dts/tune-dts.json"
SPA_CFG="$GEN_DIR/tune-spa/tune-spa.json"
WEB_CFG="$GEN_DIR/tune-web/tune-web.json"

echo "=== DDS tune chain start (rounds=$ROUNDS interval=${INTERVAL_US}us run=${RUN_S}s) ==="
"$DTS_BIN" "$DTS_CFG" > /tmp/tune_dts.log 2>&1 &
DTS_PID=$!
sleep 4                              # dts 上电 + 静态发现

"$WEB_BIN" "$WEB_CFG" "$OUT_CSV" "$RUN_S" > /tmp/tune_web.log 2>&1 &
WEB_PID=$!
sleep 2                              # web 订阅 + 发现完成

"$SPA_BIN" "$SPA_CFG" "$ROUNDS" "$INTERVAL_US" > /tmp/tune_spa.log 2>&1
SPA_RC=$?

wait "$WEB_PID" 2>/dev/null          # web 收 RUN_S 后自行退出（析构 segfault 忽略）
kill -INT "$DTS_PID" 2>/dev/null
sleep 1
kill -9 "$DTS_PID" 2>/dev/null

echo "--- spa-mock ---"; grep -E "sent|handshake" /tmp/tune_spa.log || true
echo "--- web-mock ---"; grep -E "report=" /tmp/tune_web.log || true
echo "--- dts data recv (reader[3]=msg3 raw) ---"; grep "reader\[3\]" /tmp/tune_dts.log || true

CSV_LINES=$(wc -l < "$OUT_CSV" 2>/dev/null || echo 0)
echo "=== result: spa_rc=$SPA_RC csv_lines=$CSV_LINES (out=$OUT_CSV) ==="
# PASS：spa 发成功 + web 收到上报（CSV > 1 表头行）
[ "$SPA_RC" -eq 0 ] && [ "$CSV_LINES" -gt 1 ]
