#!/usr/bin/env bash
# plain 定长通道冒烟：plain_pub（定长 1024 帧）-> plain_dts（订阅计数）。
# 验证 FixedBytesType 通道功能：定长校验 / 收发（DataSharing 可用时 loan 零拷贝，
# 否则回退拷贝路径——两种环境都必须收到全量帧）。
# 用法：run_plain.sh <dts_bin> <pub_bin> <gen_dir>
set -u

DTS_BIN=$1
PUB_BIN=$2
GEN_DIR=$3
COUNT=${4:-10}

DTS_CFG="$GEN_DIR/plain-dts/plain-dts.json"
PUB_CFG="$GEN_DIR/plain-pub/plain-pub.json"

echo "=== plain fixed-size channel smoke (count=$COUNT) start ==="
"$DTS_BIN" "$DTS_CFG" > /tmp/plain_dts.log 2>&1 &
DTS_PID=$!
sleep 3                              # dts 上电 + 发现

"$PUB_BIN" "$PUB_CFG" "$COUNT" 1024 > /tmp/plain_pub.log 2>&1
PUB_RC=$?

sleep 2
kill -INT "$DTS_PID" 2>/dev/null
wait "$DTS_PID" 2>/dev/null

echo "--- plain-pub ---"; grep -E "sent|loan|fallback" /tmp/plain_pub.log || true
echo "--- plain-dts (type/subscribed/recv) ---"
grep -E "type up|subscribed|recv_total" /tmp/plain_dts.log || true

RECV=$(grep -oE "recv_total=[0-9]+" /tmp/plain_dts.log | grep -oE "[0-9]+" | head -1)
RECV=${RECV:-0}
echo "=== result: pub_rc=$PUB_RC dts_recv=$RECV (expect $COUNT) ==="
[ "$PUB_RC" -eq 0 ] && [ "$RECV" -eq "$COUNT" ]
