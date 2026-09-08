#!/usr/bin/env bash
# web-server 功能链路编排：dts 上报 → kafka 沉淀 → HTTP/SSE 观测。
# 链路：tune_dts + spa_mock_proc（发 N 帧 raw）+ web_server（订阅 DTS_data_4 →
#       produce dts.report → consumer → SSE/HTTP）。
# 断言：spa 全发成功 + /api/stats 总帧数 ≥1 + kafka topic 水位 ≥1（沉淀真实发生）。
# 环境自适应：podman 或 Redpanda 容器不可用 → exit 77（ctest Skipped）。
# 用法：run_web.sh <dts_bin> <spa_bin> <web_bin> <gen_dir> [rounds=30]
set -u

DTS_BIN=$1
SPA_BIN=$2
WEB_BIN=$3
GEN_DIR=$4
ROUNDS=${5:-30}
INTERVAL_US=${6:-100000}   # 100ms：发送窗口覆盖发现/匹配延迟（沙箱可达数秒）

# cd 到观测页目录前统一转绝对路径（POST_BUILD 拷贝在可执行目录 ./web）
DTS_BIN=$(readlink -f "$DTS_BIN")
SPA_BIN=$(readlink -f "$SPA_BIN")
WEB_BIN=$(readlink -f "$WEB_BIN")
GEN_DIR=$(readlink -f "$GEN_DIR")

DTS_CFG="$GEN_DIR/tune-dts/tune-dts.json"
SPA_CFG="$GEN_DIR/tune-spa/tune-spa.json"
WEB_CFG="$GEN_DIR/tune-web2/tune-web2.json"

# ---- 环境探测：podman + Redpanda（不可用 = SKIP 77）----
if ! command -v podman >/dev/null 2>&1; then
    echo "SKIP: podman not available (web-server needs kafka broker)"
    exit 77
fi
bash "$(dirname "$0")/../../tools/redpanda_up.sh" || { echo "SKIP: redpanda unavailable"; exit 77; }

echo "=== web-server functional chain (rounds=$ROUNDS) start ==="
# 受限网络自适应：RTPS 报文钉在 MTU 内（RTPS 层分片，不依赖 IP 分片——WSL mirrored 环境
# IP 分片重组不通会让多端点 SEDP 公告整批丢失，见 detmw_fastdds.cpp DETMW_UDP_MTU 注释）
export DETMW_UDP_MTU=${DETMW_UDP_MTU:-1400}
# 观测页工作目录 = 可执行目录（CMake POST_BUILD 拷贝 ./web）
cd "$(dirname "$WEB_BIN")"

# 清空 topic 使水位从 0 起：结束时断言 水位 == DDS 收帧数（每帧都沉淀，强断言）
podman exec dts-redpanda rpk topic delete dts.report >/dev/null 2>&1 || true
"$WEB_BIN" "$WEB_CFG" localhost:9092 8080 dts.report ./web > /tmp/web_server.log 2>&1 &
WEB_PID=$!
sleep 2
if ! kill -0 "$WEB_PID" 2>/dev/null; then
    echo "--- web_server failed to start ---"
    tail -5 /tmp/web_server.log
    exit 1
fi

"$DTS_BIN" "$DTS_CFG" > /tmp/web_dts.log 2>&1 &
DTS_PID=$!
sleep 4

# 小帧降低环境网络压力（32K UDP 在部分沙箱不可达）；发现窗口内重发自愈
DTS_RAW_LEN=1024 "$SPA_BIN" "$SPA_CFG" "$ROUNDS" "$INTERVAL_US" > /tmp/web_spa.log 2>&1
SPA_RC=$?

sleep 3   # 等 kafka 投递/消费/统计闭环

# ---- 断言（杀进程前先取在线快照）----
STATS=$(curl -s --max-time 3 http://localhost:8080/api/stats 2>/dev/null || true)

kill -INT "$WEB_PID" "$DTS_PID" 2>/dev/null
sleep 1
kill -9 "$WEB_PID" "$DTS_PID" 2>/dev/null

FRAMES=$(echo "$STATS" | grep -oE '"total_frames":[0-9]+' | grep -oE '[0-9]+' || echo 0)
# rpk 分区表：PARTITION LEADER EPOCH REPLICAS LOG-START-OFFSET HIGH-WATERMARK → 累加各分区高水位
HI=$(podman exec dts-redpanda rpk topic describe dts.report -p 2>/dev/null \
     | awk 'NR>1 && $1 ~ /^[0-9]+$/ {sum+=$NF} END{print sum+0}')
[ -z "$HI" ] && HI=0

echo "--- spa ---";   tail -2 /tmp/web_spa.log
echo "--- web ---";   tail -4 /tmp/web_server.log
echo "--- kafka ---"; podman exec dts-redpanda rpk topic describe dts.report -p 2>/dev/null | head -6
echo "=== result: spa_rc=$SPA_RC frames=$FRAMES kafka_hi=$HI (expect hi==frames>=1) ==="
[ "$SPA_RC" -eq 0 ] && [ "$FRAMES" -ge 1 ] && [ "$HI" -eq "$FRAMES" ]
