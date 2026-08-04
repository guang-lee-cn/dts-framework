#!/usr/bin/env bash
# data 基准（B）：spa 发 ≤32K struct(msg3) -> data 线程收 + 4字节拷贝模拟业务下限
# 观测：dts data_3 reader 收数（data 线程接收能力）
# 用法：run_data_bench.sh <dts_bin> <gen_bin> <gen_dir>
set -u

DTS=$1
GEN=$2
GEN_DIR=$3

echo "=== data 线程接收基准（≤32K struct，4字节拷贝模拟业务下限）==="
echo ""
echo "| 发送数 | 速率 | data reader 收 | 丢包 |"
echo "|---|---|---|---|"
for count in 50 100 200 500 1000; do
  for rate in "0:紧突发" "1000:1ms" "5000:5ms"; do
    delay=${rate%%:*}; rname=${rate##*:}
    "$DTS" "$GEN_DIR/perf-dts/perf-dts.json" > /tmp/db_dts.log 2>&1 &
    D=$!
    sleep 3
    timeout 40 "$GEN" "$GEN_DIR/perf-gen/perf-gen.json" "$count" 32768 3 "$delay" data > /tmp/db.log 2>&1
    kill "$D" 2>/dev/null
    recv=$(grep "reader\[3\]" /tmp/db_dts.log | grep -oE "recv_total=[0-9]+" | cut -d= -f2)
    recv=${recv:-0}
    loss=$((count - recv))
    [ $loss -lt 0 ] && loss=0
    echo "| $count | $rname | ${recv:-NA} | $loss |"
  done
done
echo ""
echo "=== 完成 ==="
