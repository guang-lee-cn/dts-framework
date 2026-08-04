#!/usr/bin/env bash
# 跨进程测试链路：拉起 dts_test + nfoam_test + spa_test，
# 验证静态发现（EDP=STATIC + staticdiscovery.xml）下相互收发：
#   nfoam -> task(msg1) -> task 回包 msg2 -> nfoam   （往返）
#   spa   -> data(msg3) <32k 大帧                      （数据通路）
# 用法：run_cross_test.sh <dts_bin> <nfoam_bin> <spa_bin> <gen_dir>
set -u

DTS_BIN=$1
NFOAM_BIN=$2
SPA_BIN=$3
GEN_DIR=$4

DTS_CFG="$GEN_DIR/dts-test/dts-test.json"
NFOAM_CFG="$GEN_DIR/nfoam/nfoam.json"
SPA_CFG="$GEN_DIR/spa/spa.json"

echo "=== cross-process test start ==="
"$DTS_BIN" "$DTS_CFG" > /tmp/dts_test.log 2>&1 &
DTS_PID=$!

# 等 dts 上电 + 静态发现（PDP/EDP 握手）
sleep 3

"$NFOAM_BIN" "$NFOAM_CFG" > /tmp/nfoam_test.log 2>&1
NFOAM_RC=$?

"$SPA_BIN" "$SPA_CFG" > /tmp/spa_test.log 2>&1
SPA_RC=$?

kill "$DTS_PID" 2>/dev/null
wait "$DTS_PID" 2>/dev/null

echo "--- dts log (feed/subscribed) ---"
grep -E "agent data feed|thread up|subscribed|error" /tmp/dts_test.log | head -20 || true
echo "--- nfoam log ---"
cat /tmp/nfoam_test.log || true
echo "--- spa log ---"
cat /tmp/spa_test.log || true

echo "=== result: nfoam_rc=$NFOAM_RC spa_rc=$SPA_RC ==="
if [ "$NFOAM_RC" -eq 0 ] && [ "$SPA_RC" -eq 0 ]; then
    echo "PASS"
    exit 0
fi
echo "FAIL"
exit 1
