#!/usr/bin/env bash
# task 双向收发基准（A）：nfoam 发 32K JSON 配置 -> task 简析+响应 -> nfoam 量测
# 扫 count(含>64 变更) × rate，测丢包临界点 + 往返耗时
# 用法：run_task_bench.sh <dts_bin> <nfoam_bin> <gen_dir>
set -u

DTS=$1
NFOAM=$2
GEN_DIR=$3

echo "=== task 双向收发基准（32K JSON，task parse+响应）==="
echo ""
echo "| 发送数 | 速率 | 响应 | 丢包 | 双投 | 往返avg(us) | 往返p99 |"
echo "|---|---|---|---|---|---|---|"
for count in 50 100 200; do
  for rate in "0:紧突发" "1000:1ms" "10000:10ms" "100000:100ms"; do
    delay=${rate%%:*}; rname=${rate##*:}
    "$DTS" "$GEN_DIR/perf-dts/perf-dts.json" > /tmp/tb_dts.log 2>&1 &
    D=$!
    sleep 3
    timeout 40 "$NFOAM" "$GEN_DIR/perf-gen/perf-gen.json" "$count" 32768 "$delay" > /tmp/tb.log 2>&1
    kill "$D" 2>/dev/null
    resp=$(grep -oE "response=[0-9]+" /tmp/tb.log | grep -oE "[0-9]+")
    loss=$(grep -oE "loss=[0-9]+" /tmp/tb.log | grep -oE "[0-9]+")
    dup=$(grep -oE "dup\(双投\)=[0-9]+" /tmp/tb.log | grep -oE "[0-9]+")
    avg=$(grep -oE "avg=[0-9.]+" /tmp/tb.log | cut -d= -f2)
    p99=$(grep -oE "p99=[0-9]+" /tmp/tb.log | cut -d= -f2)
    echo "| $count | $rname | ${resp:-NA} | ${loss:-NA} | ${dup:-NA} | ${avg:-NA} | ${p99:-NA} |"
  done
done
echo ""
echo "=== 完成 ==="
