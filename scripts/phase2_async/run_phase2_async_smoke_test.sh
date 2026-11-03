#!/usr/bin/env bash

# 用途：先用小请求量验证第二阶段的非阻塞 PostgreSQL 链路是否正确。
# 测试 READ、INSERT、UPDATE、DELETE 四种操作，每种操作默认执行 1,000 次。
# 默认客户端：4 个 Worker、每条连接 Window=8，总在途 RPC=32；服务端 4 个 IO EventLoop，对应 4 条 PostgreSQL 连接。
# 每种操作开始前重置压测表，每种操作单独启动一次服务端和客户端，结束后检查对应表的记录数。
# 结果保存在 Linux 的 ~/rpc-bench/ordersystem_phase2_async/ 下；不会把结果混进源码目录。
# 注意：脚本会重置专用压测数据库中的测试表，不要连接真实业务数据库。

set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PHASE2_IO_THREADS="${SMOKE_IO_THREADS:-4}"
PHASE2_WORKERS="${SMOKE_WORKERS:-4}"
PHASE2_WINDOW="${SMOKE_WINDOW:-8}"
PHASE2_PIN_CPUS=0
source "$SCRIPT_DIR/phase2_test_common.sh"

REQUESTS="${SMOKE_REQUESTS:-1000}"
TIMEOUT_MS="${SMOKE_TIMEOUT_MS:-10000}"
CONNECT_TIMEOUT_MS="${SMOKE_CONNECT_TIMEOUT_MS:-10000}"
MAX_SECONDS="${SMOKE_MAX_SECONDS:-120}"

phase2_init "smoke"
phase2_check_number SMOKE_REQUESTS "$REQUESTS" "$PHASE2_WORKERS" 1000000
phase2_check_number SMOKE_TIMEOUT_MS "$TIMEOUT_MS" 1 120000
phase2_check_number SMOKE_CONNECT_TIMEOUT_MS "$CONNECT_TIMEOUT_MS" 1 120000
phase2_check_number SMOKE_MAX_SECONDS "$MAX_SECONDS" 1 3600

CSV="$PHASE2_RUN_DIR/results.csv"

echo "============================================================"
echo "OrderSystem Phase 2 async PostgreSQL smoke test"
echo "database=$DB_NAME at $DB_HOST:$DB_PORT"
echo "RPC server=$RPC_HOST:$RPC_PORT"
echo "server: io_threads=$PHASE2_IO_THREADS, db_connections=$PHASE2_IO_THREADS, mode=event_loop_nonblocking"
echo "client: workers=$PHASE2_WORKERS, window=$PHASE2_WINDOW, total_inflight=$((PHASE2_WORKERS * PHASE2_WINDOW))"
echo "requests_per_operation=$REQUESTS"
echo "output=$PHASE2_RUN_DIR"
echo "============================================================"

for operation in read insert update delete; do
    echo
    echo "============================================================"
    echo "operation=$operation requests=$REQUESTS"
    echo "============================================================"

    phase2_reset_tables "$operation"
    phase2_start_server "$operation"

    log="$PHASE2_RUN_DIR/${operation}.log"
    phase2_run_client "$operation" "$REQUESTS" "$PHASE2_WORKERS" "$PHASE2_WINDOW" \
        "$TIMEOUT_MS" "$CONNECT_TIMEOUT_MS" "$MAX_SECONDS" "$CSV" "$log"

    phase2_verify_operation "$operation" "$REQUESTS"
    phase2_stop_server
done

{
    echo "OrderSystem Phase 2 async smoke test"
    echo "run_id=$PHASE2_RUN_ID"
    echo "database=$DB_NAME"
    echo "io_threads=$PHASE2_IO_THREADS"
    echo "db_connections=$PHASE2_IO_THREADS"
    echo "workers=$PHASE2_WORKERS"
    echo "window=$PHASE2_WINDOW"
    echo "total_inflight=$((PHASE2_WORKERS * PHASE2_WINDOW))"
    echo "requests_per_operation=$REQUESTS"
    echo
    for operation in read insert update delete; do
        echo "===== $operation ====="
        phase2_print_metrics "$PHASE2_RUN_DIR/${operation}.log"
        echo
    done
} > "$PHASE2_RUN_DIR/summary.txt"

echo
echo "============================================================"
echo "ALL PHASE 2 ASYNC SMOKE TESTS PASSED"
echo "CSV: $CSV"
echo "Logs: $PHASE2_RUN_DIR"
echo "============================================================"
