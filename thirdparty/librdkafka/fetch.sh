#!/usr/bin/env bash
# librdkafka 免 root 解包：apt download（走系统已配镜像源）+ dpkg -x 到本目录。
# 产物 thirdparty/librdkafka/usr/{include,lib}（.gitignore，不入库；删除后重跑本脚本即可）
set -euo pipefail
cd "$(dirname "$0")"

echo "[fetch] downloading librdkafka debs (via apt mirror)..."
apt-get download librdkafka1 librdkafka-dev

echo "[fetch] extracting..."
mkdir -p usr
for deb in librdkafka1_*.deb librdkafka-dev_*.deb; do
  [ -e "$deb" ] || continue
  dpkg -x "$deb" .
done
rm -f librdkafka1_*.deb librdkafka-dev_*.deb

[ -f usr/include/librdkafka/rdkafka.h ] || { echo "[fetch] FAILED: header missing"; exit 1; }
echo "[fetch] done: $(ls usr/lib/x86_64-linux-gnu/librdkafka.so)"
