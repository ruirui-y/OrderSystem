#!/usr/bin/env bash

# 用途：只测试 READ，逐步提高每条 RPC 连接的 Window，观察第二阶段的吞吐量和长尾延迟如何变化。
# 默认 Window 顺序：1、2、4、8、16、32、64、128、256、512；4 个客户端 Worker 对应 4 条 RPC 连接。
# 服务端固定使用 4 个 IO EventLoop，也就是 4 条 PostgreSQL 连接；每个 EventLoop 的数据库连接仍然一次只执行一条 SQL。
# 每个 Window 都启动一个全新的服务端和客户端，默认每档执行 300,000 次 READ。
# 这个测试用于观察并发窗口增加后，连接内部排队时间、数据库调用耗时和 RPC 长尾延迟的变化。
# 结果保存在 Linux 的 ~/rpc-bench/ordersystem_phase2_async/ 下，之后用 sync_phase2_results.ps1 拉回 Windows。
# 本脚本只读 bench_read_records，不重置或修改测试数据。

set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PHASE2_IO_THREADS="${SWEEP_IO_THREADS:-4}"
PHASE2_WORKERS="${SWEEP_WORKERS:-4}"
PHASE2_WINDOW=1
PHASE2_PIN_CPUS="${SWEEP_PIN_CPUS:-1}"
PHASE2_SERVER_CPU="${SWEEP_SERVER_CPU:-0-3}"
PHASE2_CLIENT_CPU="${SWEEP_CLIENT_CPU:-4-7}"
source "$SCRIPT_DIR/phase2_test_common.sh"

WINDOWS_TEXT="${SWEEP_WINDOWS:-1 2 4 8 16 32 64 128 256 512}"
REQUESTS="${SWEEP_REQUESTS:-300000}"
TIMEOUT_MS="${SWEEP_TIMEOUT_MS:-10000}"
CONNECT_TIMEOUT_MS="${SWEEP_CONNECT_TIMEOUT_MS:-10000}"
MAX_SECONDS="${SWEEP_MAX_SECONDS:-600}"

phase2_init "window_sweep"
sed -i 's/^client_window=.*/client_window=varies/; s/^client_total_inflight=.*/client_total_inflight=varies/' "$PHASE2_RUN_DIR/environment.txt"
phase2_check_number SWEEP_REQUESTS "$REQUESTS" "$PHASE2_WORKERS" 100000000
phase2_check_number SWEEP_TIMEOUT_MS "$TIMEOUT_MS" 1 120000
phase2_check_number SWEEP_CONNECT_TIMEOUT_MS "$CONNECT_TIMEOUT_MS" 1 120000
phase2_check_number SWEEP_MAX_SECONDS "$MAX_SECONDS" 1 86400

read -r -a WINDOWS <<< "$WINDOWS_TEXT"
(( ${#WINDOWS[@]} > 0 )) || phase2_fail "SWEEP_WINDOWS must contain at least one window value"

CSV="$PHASE2_RUN_DIR/results.csv"

cat >> "$PHASE2_RUN_DIR/environment.txt" <<META
sweep_windows=$WINDOWS_TEXT
requests_per_window=$REQUESTS
timeout_ms=$TIMEOUT_MS
max_seconds=$MAX_SECONDS
META

echo "============================================================"
echo "OrderSystem Phase 2 async READ window sweep"
echo "database=$DB_NAME at $DB_HOST:$DB_PORT"
echo "server: io_threads=$PHASE2_IO_THREADS, db_connections=$PHASE2_IO_THREADS"
echo "client: workers=$PHASE2_WORKERS"
echo "windows=$WINDOWS_TEXT"
echo "requests_per_window=$REQUESTS"
echo "output=$PHASE2_RUN_DIR"
echo "============================================================"

for window in "${WINDOWS[@]}"; do
    phase2_check_number SWEEP_WINDOW "$window" 1 65536
    total_inflight=$((PHASE2_WORKERS * window))

    echo
    echo "============================================================"
    echo "READ window=$window total_inflight=$total_inflight requests=$REQUESTS"
    echo "============================================================"

    phase2_start_server "read_window${window}"

    log="$PHASE2_RUN_DIR/read_window${window}.log"
    phase2_run_client read "$REQUESTS" "$PHASE2_WORKERS" "$window" \
        "$TIMEOUT_MS" "$CONNECT_TIMEOUT_MS" "$MAX_SECONDS" "$CSV" "$log"

    phase2_verify_operation read "$REQUESTS"
    phase2_stop_server
done

{
    echo "OrderSystem Phase 2 async READ window sweep"
    echo "run_id=$PHASE2_RUN_ID"
    echo "io_threads=$PHASE2_IO_THREADS"
    echo "db_connections=$PHASE2_IO_THREADS"
    echo "workers=$PHASE2_WORKERS"
    echo "windows=$WINDOWS_TEXT"
    echo "requests_per_window=$REQUESTS"
    echo
    for window in "${WINDOWS[@]}"; do
        echo "===== window $window total_inflight $((PHASE2_WORKERS * window)) ====="
        phase2_print_metrics "$PHASE2_RUN_DIR/read_window${window}.log"
        echo
    done
} > "$PHASE2_RUN_DIR/summary.txt"

echo
echo "============================================================"
echo "PHASE 2 READ WINDOW SWEEP COMPLETED"
echo "CSV: $CSV"
echo "Logs: $PHASE2_RUN_DIR"
echo "============================================================"
