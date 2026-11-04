#!/usr/bin/env bash

# 用途：先用小请求量验证第三阶段的 PostgreSQL Pipeline 链路是否正确。
# 测试 READ、INSERT、UPDATE、DELETE 四种操作，每种操作默认执行 1,000 次。
# 默认客户端：4 个 Worker、每条连接 Window=8，总在途 RPC=32；服务端 4 个 IO EventLoop，对应 4 条 PostgreSQL 连接。
# 每种操作开始前重置压测表，每种操作单独启动一次服务端和客户端，结束后检查对应表的记录数。
# 结果保存在 Linux 的 ~/rpc-bench/ordersystem_phase3_pipeline/ 下；不会把结果混进源码目录。
# 注意：脚本会重置专用压测数据库中的测试表，不要连接真实业务数据库。

set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PHASE3_IO_THREADS="${SMOKE_IO_THREADS:-4}"
PHASE3_WORKERS="${SMOKE_WORKERS:-4}"
PHASE3_WINDOW="${SMOKE_WINDOW:-8}"
PHASE3_PIN_CPUS=0
PHASE3_MAX_OUTSTANDING_REQUESTS="${SMOKE_MAX_OUTSTANDING_REQUESTS:-4096}"
source "$SCRIPT_DIR/phase3_test_common.sh"

REQUESTS="${SMOKE_REQUESTS:-1000}"
TIMEOUT_MS="${SMOKE_TIMEOUT_MS:-10000}"
CONNECT_TIMEOUT_MS="${SMOKE_CONNECT_TIMEOUT_MS:-10000}"
MAX_SECONDS="${SMOKE_MAX_SECONDS:-120}"

phase3_init "smoke"
phase3_check_number SMOKE_REQUESTS "$REQUESTS" "$PHASE3_WORKERS" 1000000
phase3_check_number SMOKE_TIMEOUT_MS "$TIMEOUT_MS" 1 120000
phase3_check_number SMOKE_CONNECT_TIMEOUT_MS "$CONNECT_TIMEOUT_MS" 1 120000
phase3_check_number SMOKE_MAX_SECONDS "$MAX_SECONDS" 1 3600

CSV="$PHASE3_RUN_DIR/results.csv"

echo "============================================================"
echo "OrderSystem Phase 3 async PostgreSQL smoke test"
echo "database=$DB_NAME at $DB_HOST:$DB_PORT"
echo "RPC server=$RPC_HOST:$RPC_PORT"
echo "server: io_threads=$PHASE3_IO_THREADS, db_connections=$PHASE3_IO_THREADS, mode=event_loop_nonblocking"
echo "client: workers=$PHASE3_WORKERS, window=$PHASE3_WINDOW, total_inflight=$((PHASE3_WORKERS * PHASE3_WINDOW))"
echo "requests_per_operation=$REQUESTS"
echo "output=$PHASE3_RUN_DIR"
echo "============================================================"

for operation in read insert update delete; do
    echo
    echo "============================================================"
    echo "operation=$operation requests=$REQUESTS"
    echo "============================================================"

    phase3_reset_tables "$operation"
    phase3_start_server "$operation"

    log="$PHASE3_RUN_DIR/${operation}.log"
    phase3_run_client "$operation" "$REQUESTS" "$PHASE3_WORKERS" "$PHASE3_WINDOW" \
        "$TIMEOUT_MS" "$CONNECT_TIMEOUT_MS" "$MAX_SECONDS" "$CSV" "$log"

    phase3_verify_operation "$operation" "$REQUESTS"
    phase3_stop_server
done

{
    echo "OrderSystem Phase 3 async smoke test"
    echo "run_id=$PHASE3_RUN_ID"
    echo "database=$DB_NAME"
    echo "io_threads=$PHASE3_IO_THREADS"
    echo "db_connections=$PHASE3_IO_THREADS"
    echo "workers=$PHASE3_WORKERS"
    echo "window=$PHASE3_WINDOW"
    echo "total_inflight=$((PHASE3_WORKERS * PHASE3_WINDOW))"
    echo "requests_per_operation=$REQUESTS"
    echo
    for operation in read insert update delete; do
        echo "===== $operation ====="
        phase3_print_metrics "$PHASE3_RUN_DIR/${operation}.log"
        echo
    done
} > "$PHASE3_RUN_DIR/summary.txt"

echo
echo "============================================================"
echo "ALL PHASE 3 PIPELINE SMOKE TESTS PASSED"
echo "CSV: $CSV"
echo "Logs: $PHASE3_RUN_DIR"
echo "============================================================"
