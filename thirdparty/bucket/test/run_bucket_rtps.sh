#!/usr/bin/env bash
# bucket 端到端 RTPS 测试编排：pub/sub 双进程跑完整 FastDDS 收发（桶替代内置 SHM）。
# 环境依赖：POSIX shm（/dev/shm）。不可用（CI 沙箱/容器）→ 返回 77（ctest SKIP_RETURN_CODE 跳过）。
# 用法：run_bucket_rtps.sh <bucket_rtps_test> <count> <size>
set -u

TEST_BIN=$1
COUNT=${2:-200}
SIZE=${3:-32768}

# POSIX shm 可用性探测：建文件即删（bucket 用 shm_open，同 /dev/shm 权限域）
if ! (umask 0; : > /dev/shm/.dts_shm_probe 2>/dev/null); then
    echo "SKIP: /dev/shm not writable (bucket transport needs POSIX shm)"
    exit 77
fi
rm -f /dev/shm/.dts_shm_probe

# 内存护栏（工具载体，2026-08-20 OOM 教训：收缓冲曾按 UINT32_MAX 分配，12GB/进程崩机）。
# 3GB 虚拟上限 ≈ 实测 RSS(~200MB) 的 10 倍余量；不够用时先查泄漏，不是调大帽子
ulimit -v 3145728 2>/dev/null || true

echo "=== bucket RTPS e2e (count=$COUNT size=$SIZE) start ==="
"$TEST_BIN" sub "$COUNT" "$SIZE" 30 > /tmp/bucket_sub.log 2>&1 &
SUB_PID=$!
sleep 2

"$TEST_BIN" pub "$COUNT" "$SIZE" 0 3 > /tmp/bucket_pub.log 2>&1
PUB_RC=$?
wait "$SUB_PID"
SUB_RC=$?

echo "--- bucket pub ---"; tail -4 /tmp/bucket_pub.log
echo "--- bucket sub ---"; tail -6 /tmp/bucket_sub.log
echo "=== result: pub_rc=$PUB_RC sub_rc=$SUB_RC ==="
[ "$PUB_RC" -eq 0 ] && [ "$SUB_RC" -eq 0 ]
