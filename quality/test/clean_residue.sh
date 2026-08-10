#!/usr/bin/env bash
# 清理 DDS 测试残留：杀残留进程 + 清 /dev/shm 的 FastDDS 共享段（kill -9 不释放 SHM 会累积撑爆内存）。
# 用法：clean_residue.sh [确认]（加任意参数跳过确认；默认交互确认）
# 用途：测试卡死/断连后手动恢复，或怀疑 SHM 残留时清。
set -u

if [ "$#" -eq 0 ]; then
    echo "将杀: tune_dts/spa_mock_proc/webserver_mock/dts_test/perf_gen/perf_sub/spa_test/nfoam_test/bench_nfoam"
    echo "将清: /dev/shm 下 fastrtps_* 段（FastDDS SHM 残留）"
    printf "继续? [y/N] "; read -r ans
    [ "$ans" = "y" ] || { echo "取消"; exit 0; }
fi

echo "=== 杀残留测试进程 ==="
for nm in tune_dts spa_mock_proc webserver_mock dts_test cpf_dts dpf_dts perf_gen perf_sub spa_test nfoam_test bench_nfoam dts_integration_test; do
    pkill -9 -x "$nm" 2>/dev/null && echo "  killed $nm"
done

echo "=== 清 /dev/shm FastDDS SHM 残留 ==="
cnt=0
for f in /dev/shm/fastrtps_* /dev/shm/shm_*; do
    [ -e "$f" ] || continue
    rm -f "$f" && cnt=$((cnt + 1))
done
echo "  清理 $cnt 个 SHM 文件"

echo "=== /dev/shm 现状 ==="
du -sh /dev/shm 2>/dev/null
echo "=== 系统内存 ==="
free -h | head -2
