#!/usr/bin/env bash
# DDS 安全包络扫描：摸清链路安全上限，业务代码耗时另加冗余
# Q1: data 线程周期同步极限（32K 数据按周期扫，找最小安全周期）
# Q2: task 线程突发处理极限（配置突发按规模扫，找安全突发上限）
# 超限监控：每格记录 dts 是否存活（线程栈/崩溃）
# 用法：run_envelope.sh <dts_bin> <gen_bin> <sub_bin> <gen_dir>
set -u

DTS=$1
GEN=$2
SUB=$3
GEN_DIR=$4
RUN="$(dirname "$0")/run_perf.sh"

# 单格跑测：返回 "loss msg4recv dts_alive"
# loss = 去重后丢包(task_hop)，msg4recv = data 回声端到端收数，dts_alive = dts 全程未崩
cell() {
    local count=$1 size=$2 delay=$3
    local out=$($RUN "$DTS" "$GEN" "$SUB" "$GEN_DIR" "$count" /tmp/env.csv 3 "$size" "$delay" 2>&1)
    local loss=$(echo "$out" | sed -nE 's/.*loss task_hop=([0-9]+).*/\1/p' | head -1)
    local recv=$(echo "$out" | sed -nE 's/.*msg4_recv=([0-9]+).*/\1/p' | head -1)
    # dts 是否被 kill 前崩溃：perf_dts.log 无 segfault/core
    local alive="Y"
    grep -qE "Segmentation|core dumped|Aborted|fatal" /tmp/perf_dts.log 2>/dev/null && alive="N"
    echo "${loss:-NA} ${recv:-NA} $alive"
}

echo "=== DDS 安全包络扫描 ==="
echo ""
echo "## Q1: data 线程周期同步极限（32K × 30 条，按周期扫）"
echo ""
echo "| 周期(us) | 发送 | 端到端收到 | 丢包 | dts存活 |"
echo "|---|---|---|---|---|"
for p in 0 500 1000 2000 5000 10000 20000 50000; do
    r=$(cell 30 32768 "$p")
    loss=$(echo "$r" | cut -d' ' -f1); recv=$(echo "$r" | cut -d' ' -f2); alive=$(echo "$r" | cut -d' ' -f3)
    echo "| $p | 30 | ${recv:-NA} | ${loss:-NA} | $alive |"
done

echo ""
echo "## Q2: task 线程突发处理极限（32K 紧突发，按规模扫）"
echo ""
echo "| 突发规模 | 端到端收到 | 丢包 | dts存活 |"
echo "|---|---|---|---|"
for n in 10 30 50 64 80 100 200; do
    r=$(cell "$n" 32768 0)
    loss=$(echo "$r" | cut -d' ' -f1); recv=$(echo "$r" | cut -d' ' -f2); alive=$(echo "$r" | cut -d' ' -f3)
    echo "| $n | ${recv:-NA} | ${loss:-NA} | $alive |"
done

echo ""
echo "=== 完成 ==="
