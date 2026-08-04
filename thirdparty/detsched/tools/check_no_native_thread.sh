#!/bin/bash
# detsched 库提供的 CI 检查：拦截业务代码直接使用原生线程/调度 API（强制走 detsched）
# 用法：check_no_native_thread.sh <scan-dir>...
#   例：check_no_native_thread.sh /path/to/business/src /path/to/business/app
# 由业务工程自行集成到自身 CI（detsched 库不代集成）
# 豁免：thirdparty/detsched（库内部本就该用 pthread）
set -u

if [ "$#" -eq 0 ]; then
    echo "usage: $0 <scan-dir>..."
    exit 2
fi

EXCLUDE='thirdparty/detsched'

FORBIDDEN_PATTERNS=(
    'std::thread'
    'pthread_create'
    'pthread_attr_init'
    'pthread_join'
    'pthread_detach'
    'pthread_setaffinity_np'
    'pthread_setschedparam'
    'sched_setscheduler'
    'sched_setparam'
)

rc=0
for pat in "${FORBIDDEN_PATTERNS[@]}"; do
    hits=$(grep -rn "$pat" "$@" 2>/dev/null | grep -v "$EXCLUDE" || true)
    if [ -n "$hits" ]; then
        echo "FORBIDDEN '$pat' in business code (must use detsched):"
        echo "$hits"
        rc=1
    fi
done

if [ "$rc" -eq 0 ]; then
    echo "[check_no_native_thread] OK"
fi
exit $rc
