#!/usr/bin/env bash
# DDS 链路多维度压测：多进程多线程 pub/sub 稳定性 + 数据处理极限
# 维度：包长 × 速率（主链路 gen→task→data→sub）+ log 段 pub/sub + S级参考
# 用法：run_bench.sh <dts_bin> <gen_bin> <sub_bin> <gen_dir>
set -u

DTS=$1
GEN=$2
SUB=$3
GEN_DIR=$4
RUN_PERF="$(dirname "$0")/run_perf.sh"
COUNT=50   # ≤ kMaxTasks(64)，主链路不触发业务拒收，丢包纯中间件

# 单格跑测，输出解析字段：msg_s mb_s lat_task_avg lat_task_p99 lat_data_avg lat_data_p99 loss
cell() {
    local size=$1 delay=$2 wait=$3
    local out=$($RUN_PERF "$DTS" "$GEN" "$SUB" "$GEN_DIR" "$COUNT" "/tmp/bench.csv" "$wait" "$size" "$delay" 2>&1)
    local gen=$(echo "$out" | grep -oE "sent [0-9]+ x [0-9]+B packets in [0-9.]+ s -> [0-9.]+ msg/s, [0-9.]+ MB/s")
    local msg_s=$(echo "$gen" | grep -oE "\-> [0-9.]+ msg/s" | grep -oE "[0-9.]+")
    local mb_s=$(echo "$gen" | grep -oE ", [0-9.]+ MB/s" | grep -oE "[0-9.]+")
    local ta=$(echo "$out" | grep -oE "task 段耗时\(lat_task\): n=[0-9]+ min=[0-9]+ avg=[0-9.]+" | grep -oE "avg=[0-9.]+" | cut -d= -f2)
    local tp=$(echo "$out" | grep -oE "task 段耗时\(lat_task\):.* p99=[0-9]+" | grep -oE "p99=[0-9]+" | cut -d= -f2)
    local da=$(echo "$out" | grep -oE "data 段耗时\(lat_data\): n=[0-9]+ min=[0-9]+ avg=[0-9.]+" | grep -oE "avg=[0-9.]+" | cut -d= -f2)
    local dp=$(echo "$out" | grep -oE "data 段耗时\(lat_data\):.* p99=[0-9]+" | grep -oE "p99=[0-9]+" | cut -d= -f2)
    local loss=$(echo "$out" | grep -oE "loss task_hop=[0-9]+" | grep -oE "[0-9]+")
    printf "%s %s %s %s %s %s %s %s" "${msg_s:-NA}" "${mb_s:-NA}" "${ta:-NA}" "${tp:-NA}" "${da:-NA}" "${dp:-NA}" "${loss:-NA}" "${gen:-ERR}"
}

echo "=== DDS 链路多维度压测 ==="
echo ""
echo "## 1. 主链路 task→data 吞吐/时延/丢包矩阵 (count=$COUNT, ≤64 业务无拒)"
echo ""
printf "| 包长 | 速率 | 写入吞吐(msg/s) | 吞吐(MB/s) | task段avg(us) | task段p99 | data段avg(us) | data段p99 | 丢包 |\n"
printf "|---|---|---|---|---|---|---|---|---|\n"
for size in 256 4096 32768; do
  for rate in "0:突发" "1000:1ms" "5000:5ms"; do
    delay=${rate%%:*}; rname=${rate##*:}
    r=$(cell $size $delay 3)
    msg_s=$(echo "$r" | cut -d' ' -f1); mb_s=$(echo "$r" | cut -d' ' -f2)
    ta=$(echo "$r" | cut -d' ' -f3); tp=$(echo "$r" | cut -d' ' -f4)
    da=$(echo "$r" | cut -d' ' -f5); dp=$(echo "$r" | cut -d' ' -f6)
    loss=$(echo "$r" | cut -d' ' -f7)
    printf "| %sB | %s | %s | %s | %s | %s | %s | %s | %s |\n" \
      "$size" "$rname" "$msg_s" "$mb_s" "$ta" "$tp" "$da" "$dp" "$loss"
  done
done

echo ""
echo "## 2. log 线程 pub/sub 验证（gen→log→echo→sub，三线程都 pub/sub）"
echo ""
r=$(cell 256 0 3)
echo "主链路已完成；log 段单独跑（gen 发 log 采集 → log 线程回抛 → sub）："
out=$($RUN_PERF "$DTS" "$GEN" "$SUB" "$GEN_DIR" 20 "/tmp/bench_log.csv" 3 256 1000 log 2>&1)
echo "$out" | grep -E "log 段收发|perf-gen\] sent" | sed 's/^/  /'

echo ""
echo "## 3. S 级参考（真实工况：1条/s × 32K）"
echo ""
out=$($RUN_PERF "$DTS" "$GEN" "$SUB" "$GEN_DIR" 5 "/tmp/bench_s.csv" 3 32768 1000000 2>&1)
echo "$out" | grep -E "sent|loss|task 段|data 段|PASS|FAIL" | sed 's/^/  /'

echo ""
echo "=== 完成 ==="
