#!/usr/bin/env bash

# 用途：给第二阶段的测试脚本提供公共函数，三个测试脚本都会调用这里。
# 第二阶段服务端参数只有 RPC 端口和 IO EventLoop 数量；每个 EventLoop 自己持有一条 PostgreSQL 连接。
# 测试结果保存在 Linux 的 ~/rpc-bench/ordersystem_phase2_async/ 下，不放进源码目录，避免被 VS 源码同步覆盖。

set -Eeuo pipefail

PHASE2_SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PHASE2_ROOT="$(cd "$PHASE2_SCRIPT_DIR/../.." && pwd)"
PHASE2_BIN="$PHASE2_ROOT/build/linux-release/bin"
PHASE2_SERVER="$PHASE2_BIN/order_db_benchmark_server"
PHASE2_CLIENT="$PHASE2_BIN/order_db_benchmark_client"
PHASE2_RESET="$PHASE2_ROOT/scripts/common/reset_benchmark_tables.sh"

RPC_HOST="${RPC_HOST:-127.0.0.1}"
RPC_PORT="${RPC_PORT:-9000}"
DB_HOST="${DB_HOST:-127.0.0.1}"
DB_PORT="${DB_PORT:-5432}"
DB_USER="${DB_USER:-ordersystem_bench}"
DB_NAME="${DB_NAME:-ordersystem_bench}"

PHASE2_IO_THREADS="${PHASE2_IO_THREADS:-4}"
PHASE2_WORKERS="${PHASE2_WORKERS:-4}"
PHASE2_WINDOW="${PHASE2_WINDOW:-8}"
PHASE2_PIN_CPUS="${PHASE2_PIN_CPUS:-0}"
PHASE2_SERVER_CPU="${PHASE2_SERVER_CPU:-0-3}"
PHASE2_CLIENT_CPU="${PHASE2_CLIENT_CPU:-4-7}"
PHASE2_OUTPUT_ROOT="${PHASE2_OUTPUT_ROOT:-$HOME/rpc-bench/ordersystem_phase2_async}"

PHASE2_SERVER_PID=""
PHASE2_RUN_ID=""
PHASE2_RUN_DIR=""

phase2_fail()
{
    echo "ERROR: $*" >&2
    exit 1
}

phase2_check_number()
{
    local name="$1"
    local value="$2"
    local minimum="$3"
    local maximum="$4"

    [[ "$value" =~ ^[0-9]+$ ]] || phase2_fail "$name must be an integer"
    local number=$((10#$value))
    (( number >= minimum && number <= maximum )) || phase2_fail "$name must be between $minimum and $maximum"
}

phase2_port_is_listening()
{
    ss -ltnH "sport = :$RPC_PORT" 2>/dev/null | grep -q .
}

phase2_query()
{
    psql --no-psqlrc --set ON_ERROR_STOP=1 \
        --host="$DB_HOST" --port="$DB_PORT" \
        --username="$DB_USER" --dbname="$DB_NAME" \
        --tuples-only --no-align --command="$1"
}

phase2_init()
{
    local test_name="$1"

    [[ -x "$PHASE2_SERVER" ]] || phase2_fail "server executable not found: $PHASE2_SERVER; build the Linux Release target first"
    [[ -x "$PHASE2_CLIENT" ]] || phase2_fail "client executable not found: $PHASE2_CLIENT; build the Linux Release target first"
    [[ -f "$PHASE2_RESET" ]] || phase2_fail "reset script not found: $PHASE2_RESET"
    command -v psql >/dev/null 2>&1 || phase2_fail "psql was not found; install the PostgreSQL client first"
    command -v ss >/dev/null 2>&1 || phase2_fail "ss was not found; install iproute2 first"

    phase2_check_number RPC_PORT "$RPC_PORT" 1 65535
    phase2_check_number DB_PORT "$DB_PORT" 1 65535
    phase2_check_number PHASE2_IO_THREADS "$PHASE2_IO_THREADS" 1 128
    phase2_check_number PHASE2_WORKERS "$PHASE2_WORKERS" 1 128
    phase2_check_number PHASE2_WINDOW "$PHASE2_WINDOW" 1 65536

    if [[ "$DB_NAME" != "ordersystem_bench" && "${ALLOW_NON_BENCH_DB:-0}" != "1" ]]; then
        phase2_fail "this test resets benchmark tables in $DB_NAME; only ordersystem_bench is allowed. For a disposable database, explicitly set ALLOW_NON_BENCH_DB=1."
    fi

    # 已经有 ~/.pgpass 时不再要求输入密码，交给 libpq 自己读文件。
    if [[ -z "${DB_PASSWORD:-}" && ! -f "$HOME/.pgpass" ]]; then
        read -rsp "PostgreSQL password for ${DB_USER}: " DB_PASSWORD
        echo
    fi

    export DB_HOST DB_PORT DB_USER DB_NAME
    # 只有确实拿到密码才设置 PGPASSWORD。空的 PGPASSWORD 会被 libpq 当成空密码，
    # 反而挡住 ~/.pgpass 的自动读取，导致认证失败。
    if [[ -n "${DB_PASSWORD:-}" ]]; then
        export DB_PASSWORD
        export PGPASSWORD="$DB_PASSWORD"
    fi

    psql --no-psqlrc --set ON_ERROR_STOP=1 \
        --host="$DB_HOST" --port="$DB_PORT" \
        --username="$DB_USER" --dbname="$DB_NAME" \
        --tuples-only --no-align --command="SELECT 1" >/dev/null \
        || phase2_fail "cannot connect to PostgreSQL database $DB_NAME as $DB_USER"

    local read_rows
    read_rows="$(phase2_query "SELECT COUNT(*) FROM bench_read_records")"
    [[ "$read_rows" == "1000000" ]] || phase2_fail "bench_read_records has $read_rows rows; expected 1000000. Initialize sql/schema.sql and sql/seed.sql first."

    if [[ "$PHASE2_PIN_CPUS" == "1" ]]; then
        if ! command -v taskset >/dev/null 2>&1 || \
           ! taskset -c "$PHASE2_SERVER_CPU" true >/dev/null 2>&1 || \
           ! taskset -c "$PHASE2_CLIENT_CPU" true >/dev/null 2>&1; then
            echo "WARNING: CPU affinity is unavailable for $PHASE2_SERVER_CPU/$PHASE2_CLIENT_CPU; running without taskset."
            PHASE2_PIN_CPUS=0
        fi
    fi

    PHASE2_RUN_ID="${PHASE2_RUN_ID:-${test_name}_$(date +%Y%m%d_%H%M%S)}"
    PHASE2_RUN_DIR="$PHASE2_OUTPUT_ROOT/$PHASE2_RUN_ID"
    mkdir -p "$PHASE2_RUN_DIR"

    cat > "$PHASE2_RUN_DIR/environment.txt" <<META
created_at=$(date -Is)
phase=2
database=$DB_NAME
database_host=$DB_HOST
database_port=$DB_PORT
rpc_host=$RPC_HOST
rpc_port=$RPC_PORT
io_threads=$PHASE2_IO_THREADS
db_connections=$PHASE2_IO_THREADS
db_mode=event_loop_nonblocking
client_workers=$PHASE2_WORKERS
client_window=$PHASE2_WINDOW
client_total_inflight=$((PHASE2_WORKERS * PHASE2_WINDOW))
cpu_affinity=$PHASE2_PIN_CPUS
server_cpu=$PHASE2_SERVER_CPU
client_cpu=$PHASE2_CLIENT_CPU
META

    trap phase2_cleanup EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM

    if phase2_port_is_listening; then
        phase2_fail "RPC port $RPC_PORT is already occupied. Stop the existing OrderSystem server before running this test."
    fi

    echo "run_id=$PHASE2_RUN_ID"
    echo "remote_output=$PHASE2_RUN_DIR"
}

phase2_reset_tables()
{
    local label="$1"
    local log="$PHASE2_RUN_DIR/reset_${label}.log"

    if ! bash "$PHASE2_RESET" >"$log" 2>&1; then
        cat "$log" >&2
        phase2_fail "failed to reset benchmark tables before $label"
    fi
}

phase2_start_server()
{
    local label="$1"
    local log="$PHASE2_RUN_DIR/server_${label}.log"

    if phase2_port_is_listening; then
        phase2_fail "RPC port $RPC_PORT became occupied before starting the Phase 2 server"
    fi

    (
        export DB_HOST DB_PORT DB_USER DB_PASSWORD DB_NAME
        if [[ "$PHASE2_PIN_CPUS" == "1" ]]; then
            exec taskset -c "$PHASE2_SERVER_CPU" "$PHASE2_SERVER" "$RPC_PORT" "$PHASE2_IO_THREADS"
        else
            exec "$PHASE2_SERVER" "$RPC_PORT" "$PHASE2_IO_THREADS"
        fi
    ) >"$log" 2>&1 &
    PHASE2_SERVER_PID=$!

    local ready=0
    for _ in {1..300}; do
        if grep -q "db_mode=event_loop_nonblocking" "$log" && phase2_port_is_listening; then
            ready=1
            break
        fi

        if ! kill -0 "$PHASE2_SERVER_PID" 2>/dev/null; then
            cat "$log" >&2
            phase2_fail "Phase 2 server exited during startup; see $log"
        fi
        sleep 0.1
    done

    if (( ready == 0 )); then
        cat "$log" >&2
        phase2_fail "Phase 2 server did not become ready; see $log"
    fi

    grep -q "db_connections=$PHASE2_IO_THREADS" "$log" \
        || phase2_fail "server did not report the expected number of PostgreSQL connections; see $log"

    echo "RPC server started (PID=$PHASE2_SERVER_PID, IO EventLoops=$PHASE2_IO_THREADS, PostgreSQL connections=$PHASE2_IO_THREADS)."
}

phase2_stop_server()
{
    if [[ -n "$PHASE2_SERVER_PID" ]] && kill -0 "$PHASE2_SERVER_PID" 2>/dev/null; then
        kill -TERM "$PHASE2_SERVER_PID" 2>/dev/null || true

        for _ in {1..50}; do
            kill -0 "$PHASE2_SERVER_PID" 2>/dev/null || break
            sleep 0.1
        done

        if kill -0 "$PHASE2_SERVER_PID" 2>/dev/null; then
            kill -KILL "$PHASE2_SERVER_PID" 2>/dev/null || true
        fi

        wait "$PHASE2_SERVER_PID" 2>/dev/null || true
    fi

    PHASE2_SERVER_PID=""
}

phase2_cleanup()
{
    phase2_stop_server
    unset PGPASSWORD
}

phase2_run_client()
{
    local operation="$1"
    local requests="$2"
    local workers="$3"
    local window="$4"
    local timeout_ms="$5"
    local connect_timeout_ms="$6"
    local max_seconds="$7"
    local csv="$8"
    local log="$9"
    local -a command

    command=(
        "$PHASE2_CLIENT"
        --host "$RPC_HOST"
        --port "$RPC_PORT"
        --workers "$workers"
        --window "$window"
        --requests "$requests"
        --operation "$operation"
        --timeout-ms "$timeout_ms"
        --connect-timeout-ms "$connect_timeout_ms"
        --max-seconds "$max_seconds"
        --csv "$csv"
    )

    if [[ "$PHASE2_PIN_CPUS" == "1" ]]; then
        command=(taskset -c "$PHASE2_CLIENT_CPU" "${command[@]}")
    fi

    if ! "${command[@]}" >"$log" 2>&1; then
        cat "$log" >&2
        phase2_fail "$operation client test failed; see $log"
    fi

    cat "$log"
}

phase2_verify_operation()
{
    local operation="$1"
    local requests="$2"
    local table=""
    local expected=""
    local actual=""

    case "$operation" in
        read)
            table="bench_read_records"
            expected="1000000"
            ;;
        update)
            table="bench_update_records"
            expected="1000000"
            ;;
        insert)
            table="bench_insert_records"
            expected="$requests"
            ;;
        delete)
            table="bench_delete_records"
            expected="$((1000000 - requests))"
            ;;
        *)
            phase2_fail "unknown operation for verification: $operation"
            ;;
    esac

    actual="$(phase2_query "SELECT COUNT(*) FROM $table")"
    [[ "$actual" == "$expected" ]] || phase2_fail "$operation completed but $table has $actual rows; expected $expected"

    echo "PASS: $operation, $table row count=$actual"
}

phase2_print_metrics()
{
    local log="$1"
    grep -E '^(mode=|target=|wall_seconds=|latency_success_only_us:|database_call_us:|db_queue_wait_us:)' "$log" || true
}
