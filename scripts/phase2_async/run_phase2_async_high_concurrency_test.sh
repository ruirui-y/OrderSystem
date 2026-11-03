#!/usr/bin/env bash

# 用途：在与第一阶段相同的 2,048 个 RPC 在途请求下，测试第二阶段的 READ、UPDATE、INSERT、DELETE 性能。
# 默认客户端：4 个 Worker、每条连接 Window=512，总在途 RPC=2,048。
# 默认服务端：4 个 IO EventLoop，每个 Loop 独占 1 条非阻塞 PostgreSQL 连接；不再创建 DB Worker 线程池。
# 默认请求量：READ 1,000,000 次；UPDATE、INSERT、DELETE 各 200,000 次；每种操作默认测试 1 轮。
# 每轮开始前重置四张压测表，再启动全新的服务端和客户端；每轮只测试一种操作。
# 默认尝试将服务端绑到 CPU 0-3、客户端绑到 CPU 4-7；如果当前虚拟机不支持这些 CPU 编号，会自动取消绑核。
# 结果保存在 Linux 的 ~/rpc-bench/ordersystem_phase2_async/ 下，之后用 sync_phase2_results.ps1 拉回 Windows。
# 注意：脚本会重置专用压测数据库中的测试表，不要连接真实业务数据库。

set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PHASE2_IO_THREADS="${HIGH_IO_THREADS:-4}"
PHASE2_WORKERS="${HIGH_WORKERS:-4}"
PHASE2_WINDOW="${HIGH_WINDOW:-512}"
PHASE2_PIN_CPUS="${HIGH_PIN_CPUS:-1}"
PHASE2_SERVER_CPU="${HIGH_SERVER_CPU:-0-3}"
PHASE2_CLIENT_CPU="${HIGH_CLIENT_CPU:-4-7}"
source "$SCRIPT_DIR/phase2_test_common.sh"

REPEATS="${HIGH_REPEATS:-1}"
REQUESTS_READ="${HIGH_REQUESTS_READ:-1000000}"
REQUESTS_UPDATE="${HIGH_REQUESTS_UPDATE:-200000}"
REQUESTS_INSERT="${HIGH_REQUESTS_INSERT:-200000}"
REQUESTS_DELETE="${HIGH_REQUESTS_DELETE:-200000}"
TIMEOUT_MS="${HIGH_TIMEOUT_MS:-10000}"
CONNECT_TIMEOUT_MS="${HIGH_CONNECT_TIMEOUT_MS:-10000}"
MAX_SECONDS="${HIGH_MAX_SECONDS:-600}"
COOLDOWN_SECONDS="${HIGH_COOLDOWN_SECONDS:-3}"

phase2_init "high_concurrency"
phase2_check_number HIGH_REPEATS "$REPEATS" 1 10
phase2_check_number HIGH_REQUESTS_READ "$REQUESTS_READ" "$PHASE2_WORKERS" 100000000
phase2_check_number HIGH_REQUESTS_UPDATE "$REQUESTS_UPDATE" "$PHASE2_WORKERS" 100000000
phase2_check_number HIGH_REQUESTS_INSERT "$REQUESTS_INSERT" "$PHASE2_WORKERS" 100000000
phase2_check_number HIGH_REQUESTS_DELETE "$REQUESTS_DELETE" "$PHASE2_WORKERS" 1000000
phase2_check_number HIGH_TIMEOUT_MS "$TIMEOUT_MS" 1 120000
phase2_check_number HIGH_CONNECT_TIMEOUT_MS "$CONNECT_TIMEOUT_MS" 1 120000
phase2_check_number HIGH_MAX_SECONDS "$MAX_SECONDS" 1 86400
phase2_check_number HIGH_COOLDOWN_SECONDS "$COOLDOWN_SECONDS" 0 60

CSV="$PHASE2_RUN_DIR/results.csv"

cat >> "$PHASE2_RUN_DIR/environment.txt" <<META
repeats=$REPEATS
requests_read=$REQUESTS_READ
requests_update=$REQUESTS_UPDATE
requests_insert=$REQUESTS_INSERT
requests_delete=$REQUESTS_DELETE
timeout_ms=$TIMEOUT_MS
max_seconds=$MAX_SECONDS
cooldown_seconds=$COOLDOWN_SECONDS
META

echo "============================================================"
echo "OrderSystem Phase 2 async high-concurrency CRUD test"
echo "database=$DB_NAME at $DB_HOST:$DB_PORT"
echo "RPC server=$RPC_HOST:$RPC_PORT"
echo "server: io_threads=$PHASE2_IO_THREADS, db_connections=$PHASE2_IO_THREADS, mode=event_loop_nonblocking"
echo "client: workers=$PHASE2_WORKERS, window=$PHASE2_WINDOW, total_inflight=$((PHASE2_WORKERS * PHASE2_WINDOW))"
echo "requests: read=$REQUESTS_READ update=$REQUESTS_UPDATE insert=$REQUESTS_INSERT delete=$REQUESTS_DELETE"
echo "repeats=$REPEATS cpu_affinity=$PHASE2_PIN_CPUS"
echo "output=$PHASE2_RUN_DIR"
echo "============================================================"

run_operation()
{
    local operation="$1"
    local requests="$2"

    for ((repeat = 1; repeat <= REPEATS; ++repeat)); do
        echo
        echo "============================================================"
        echo "operation=$operation requests=$requests repeat=$repeat/$REPEATS"
        echo "============================================================"

        phase2_reset_tables "${operation}_run${repeat}"
        phase2_start_server "${operation}_run${repeat}"

        local log="$PHASE2_RUN_DIR/${operation}_run${repeat}.log"
        phase2_run_client "$operation" "$requests" "$PHASE2_WORKERS" "$PHASE2_WINDOW" \
            "$TIMEOUT_MS" "$CONNECT_TIMEOUT_MS" "$MAX_SECONDS" "$CSV" "$log"

        phase2_verify_operation "$operation" "$requests"
        phase2_stop_server
        sleep "$COOLDOWN_SECONDS"
    done
}

run_operation read "$REQUESTS_READ"
run_operation update "$REQUESTS_UPDATE"
run_operation insert "$REQUESTS_INSERT"
run_operation delete "$REQUESTS_DELETE"

{
    echo "OrderSystem Phase 2 async high-concurrency test"
    echo "run_id=$PHASE2_RUN_ID"
    echo "database=$DB_NAME"
    echo "io_threads=$PHASE2_IO_THREADS"
    echo "db_connections=$PHASE2_IO_THREADS"
    echo "workers=$PHASE2_WORKERS"
    echo "window=$PHASE2_WINDOW"
    echo "total_inflight=$((PHASE2_WORKERS * PHASE2_WINDOW))"
    echo "repeats=$REPEATS"
    echo
    for operation in read update insert delete; do
        echo "===== $operation ====="
        for ((repeat = 1; repeat <= REPEATS; ++repeat)); do
            echo "--- run $repeat ---"
            phase2_print_metrics "$PHASE2_RUN_DIR/${operation}_run${repeat}.log"
        done
        echo
    done
} > "$PHASE2_RUN_DIR/summary.txt"

echo
echo "============================================================"
echo "ALL PHASE 2 HIGH-CONCURRENCY CRUD TESTS PASSED"
echo "CSV: $CSV"
echo "Logs: $PHASE2_RUN_DIR"
echo "============================================================"
