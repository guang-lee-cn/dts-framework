#!/usr/bin/env bash
# bucket 段跨进程冒烟编排：1 reader + 2 writers（多写者互斥/ring 回绕/seq 全集）。
# 环境依赖：POSIX shm（/dev/shm）。不可用 → 返回 77（ctest SKIP_RETURN_CODE 跳过）。
# 用法：run_bucket_smoke.sh <bucket_smoke> [cap_mb] [total]
set -u

SMOKE_BIN=$1
CAP_MB=${2:-8}
TOTAL=${3:-500}

if ! (umask 0; : > /dev/shm/.dts_shm_probe 2>/dev/null); then
    echo "SKIP: /dev/shm not writable (bucket transport needs POSIX shm)"
    exit 77
fi
rm -f /dev/shm/.dts_shm_probe

NAME="/dts_smoke_$$"
CAP=$((CAP_MB * 1024 * 1024))
echo "=== bucket segment smoke (cap=${CAP_MB}MB total=$TOTAL) start ==="

"$SMOKE_BIN" r "$NAME" "$CAP" "$TOTAL" 30 > /tmp/bucket_smoke_r.log 2>&1 &
R_PID=$!
sleep 1

"$SMOKE_BIN" w "$NAME" "$CAP" $((TOTAL / 2)) 1024 0 100 > /tmp/bucket_smoke_w1.log 2>&1 &
W1=$!
"$SMOKE_BIN" w "$NAME" "$CAP" $((TOTAL - TOTAL / 2)) 1024 $((TOTAL / 2)) 100 > /tmp/bucket_smoke_w2.log 2>&1 &
W2=$!

wait "$W1"; W1_RC=$?
wait "$W2"; W2_RC=$?
wait "$R_PID"; R_RC=$?

echo "--- reader ---"; tail -4 /tmp/bucket_smoke_r.log
echo "--- writers ---"; tail -2 /tmp/bucket_smoke_w1.log /tmp/bucket_smoke_w2.log
echo "=== result: r=$R_RC w1=$W1_RC w2=$W2_RC ==="
[ "$R_RC" -eq 0 ] && [ "$W1_RC" -eq 0 ] && [ "$W2_RC" -eq 0 ]
