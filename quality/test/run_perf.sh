#!/usr/bin/env bash
# pubsub 能力压测：perf-gen 连发包 -> dts -> perf-sub 量测单跳往返
#   task 段（msg7 配置变更 -> task 回显 msg8，外部往返）
#   log 段（msg5 采集 -> log 回抛 msg6）
#   data 段（msg3 raw -> data 上报 msg4）需握手，由 run_slevel.sh（tune 链路）覆盖
# 用法：run_perf.sh <dts_bin> <gen_bin> <sub_bin> <gen_dir> <count> <out_csv> [gen_wait_s] [size] [delay_us] [mode]
set -u

DTS_BIN=$1
GEN_BIN=$2
SUB_BIN=$3
GEN_DIR=$4
COUNT=$5
OUT_CSV=$6
GEN_WAIT=${7:-3}
SIZE=${8:-32768}
DELAY_US=${9:-0}
MODE=${10:-task}   # task 段（msg7->msg8）/ log 段（msg5->msg6）

DTS_CFG="$GEN_DIR/perf-dts/perf-dts.json"
GEN_CFG="$GEN_DIR/perf-gen/perf-gen.json"
SUB_CFG="$GEN_DIR/perf-sub/perf-sub.json"

echo "=== pubsub perf test start (count=$COUNT, ${SIZE}B/pkt, mode=$MODE) ==="
"$DTS_BIN" "$DTS_CFG" > /tmp/perf_dts.log 2>&1 &
DTS_PID=$!
sleep 3                       # dts 上电 + 静态发现

"$SUB_BIN" "$SUB_CFG" "$OUT_CSV" "$COUNT" "$MODE" > /tmp/perf_sub.log 2>&1 &
SUB_PID=$!
sleep 2                       # sub 订阅 + 发现完成

"$GEN_BIN" "$GEN_CFG" "$COUNT" "$SIZE" "$GEN_WAIT" "$DELAY_US" "$MODE" > /tmp/perf_gen.log 2>&1
GEN_RC=$?

wait "$SUB_PID"               # sub 收到 DONE 后自行退出
SUB_RC=$?

kill "$DTS_PID" 2>/dev/null
wait "$DTS_PID" 2>/dev/null

echo "--- perf-gen ---"
cat /tmp/perf_gen.log || true
echo "--- perf-sub ---"
cat /tmp/perf_sub.log || true
echo "--- dts (feed/error) ---"
grep -E "agent data feed|error" /tmp/perf_dts.log | head -10 || true

echo "=== result: gen_rc=$GEN_RC sub_rc=$SUB_RC (out=$OUT_CSV) ==="
[ "$GEN_RC" -eq 0 ] && [ "$SUB_RC" -eq 0 ]
