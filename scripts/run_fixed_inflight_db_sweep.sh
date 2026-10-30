#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/build/linux-release/bin"
SERVER="$BIN/order_db_benchmark_server"
CLIENT="$BIN/order_db_benchmark_client"

HOST="127.0.0.1"
PORT=9000
SERVER_CPU="0-3"
CLIENT_CPU="4-7"
WORKERS=4
WINDOW=512
IO_THREADS=4
DB_WORKERS=16
REPEATS=3
TIMEOUT_MS=10000
CONNECT_TIMEOUT_MS=10000
MAX_SECONDS=180
COOLDOWN_SECONDS=3

REQUESTS_READ=30000000
REQUESTS_UPDATE=3000000
REQUESTS_INSERT=1000000
REQUESTS_DELETE=1000000

DB_HOST="${DB_HOST:-127.0.0.1}"
DB_PORT="${DB_PORT:-3306}"
DB_USER="${DB_USER:-ordersystem_bench}"
DB_NAME="${DB_NAME:-ordersystem_bench}"
if [[ -z "${DB_PASSWORD+x}" ]]; then
    read -rsp "MySQL password for ${DB_USER}: " DB_PASSWORD
    echo
fi
export DB_HOST DB_PORT DB_USER DB_PASSWORD DB_NAME

for executable in "$SERVER" "$CLIENT"; do
    if [[ ! -x "$executable" ]]; then
        echo "Executable not found: $executable" >&2
        echo "Build OrderSystem first; see docs/database-operation-benchmark.md" >&2
        exit 2
    fi
done

if ! command -v mysql >/dev/null 2>&1; then
    echo "mysql client not found. Install the MySQL command-line client first." >&2
    exit 2
fi

if ss -ltnH "sport = :$PORT" 2>/dev/null | grep -q .; then
    echo "Port $PORT is already occupied. Stop the existing server first." >&2
    exit 2
fi

STAMP="$(date +%Y%m%d_%H%M%S)"
OUTDIR="$HOME/rpc-bench/ordersystem_db_window${WINDOW}_${STAMP}"
CSV="$OUTDIR/results.csv"
mkdir -p "$OUTDIR"

cat > "$OUTDIR/environment.txt" <<META
created_at=$(date -Is)
host=$HOST
port=$PORT
workers=$WORKERS
window=$WINDOW
total_inflight=$((WORKERS * WINDOW))
io_threads=$IO_THREADS
db_workers=$DB_WORKERS
server_cpu=$SERVER_CPU
client_cpu=$CLIENT_CPU
repeats=$REPEATS
requests_read=$REQUESTS_READ
requests_update=$REQUESTS_UPDATE
requests_insert=$REQUESTS_INSERT
requests_delete=$REQUESTS_DELETE
db_host=$DB_HOST
db_port=$DB_PORT
db_name=$DB_NAME
META

SERVER_PID=""
stop_server()
{
    if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    SERVER_PID=""
}

cleanup()
{
    stop_server
}
trap cleanup EXIT
trap 'exit 130' INT TERM

start_server()
{
    local server_log="$1"

    if ss -ltnH "sport = :$PORT" 2>/dev/null | grep -q .; then
        echo "Port $PORT is occupied before starting the benchmark server." >&2
        exit 2
    fi

    DB_HOST="$DB_HOST" DB_PORT="$DB_PORT" DB_USER="$DB_USER" DB_PASSWORD="$DB_PASSWORD" DB_NAME="$DB_NAME" \
        taskset -c "$SERVER_CPU" "$SERVER" "$PORT" "$IO_THREADS" "$DB_WORKERS" \
        > "$server_log" 2>&1 &
    SERVER_PID=$!

    local ready=0
    for _ in {1..100}; do
        if ss -ltnH "sport = :$PORT" 2>/dev/null | grep -q .; then
            ready=1
            break
        fi
        if ! kill -0 "$SERVER_PID" 2>/dev/null; then
            echo "Server failed to start:" >&2
            cat "$server_log"
            exit 1
        fi
        sleep 0.1
    done

    if (( ready == 0 )); then
        echo "Server failed to listen on port $PORT." >&2
        cat "$server_log"
        exit 1
    fi
}

echo "============================================================"
echo "OrderSystem database operation benchmark"
echo "workers=$WORKERS window=$WINDOW total_inflight=$((WORKERS * WINDOW))"
echo "IO threads=$IO_THREADS database workers=$DB_WORKERS"
echo "Each run resets tables, then starts a fresh app server."
echo "output=$OUTDIR"
echo "============================================================"

run_operation()
{
    local operation="$1"
    local requests="$2"
    local repeat="$3"
    local log="$OUTDIR/${operation}_run${repeat}.log"

    echo
    echo "============================================================"
    echo "operation=$operation requests=$requests repeat=$repeat/$REPEATS"
    echo "window=$WINDOW total_inflight=$((WORKERS * WINDOW))"
    echo "============================================================"

    stop_server

    DB_HOST="$DB_HOST" DB_PORT="$DB_PORT" DB_USER="$DB_USER" DB_PASSWORD="$DB_PASSWORD" DB_NAME="$DB_NAME" \
        "$ROOT/scripts/reset_benchmark_tables.sh" \
        > "$OUTDIR/reset_${operation}_run${repeat}.log" 2>&1

    # 每次重置表后重新启动服务端，让预处理语句在表重置完成后重新创建。
    start_server "$OUTDIR/server_${operation}_run${repeat}.log"

    if ! taskset -c "$CLIENT_CPU" "$CLIENT" \
        --host "$HOST" \
        --port "$PORT" \
        --workers "$WORKERS" \
        --window "$WINDOW" \
        --requests "$requests" \
        --operation "$operation" \
        --timeout-ms "$TIMEOUT_MS" \
        --connect-timeout-ms "$CONNECT_TIMEOUT_MS" \
        --max-seconds "$MAX_SECONDS" \
        --csv "$CSV" \
        > "$log" 2>&1
    then
        cat "$log"
        echo "Benchmark failed: $operation run $repeat. See $log" >&2
        exit 1
    fi

    cat "$log"
    stop_server
    sleep "$COOLDOWN_SECONDS"
}

for operation in read update insert delete; do
    case "$operation" in
        read) requests="$REQUESTS_READ" ;;
        update) requests="$REQUESTS_UPDATE" ;;
        insert) requests="$REQUESTS_INSERT" ;;
        delete) requests="$REQUESTS_DELETE" ;;
    esac

    for ((repeat=1; repeat<=REPEATS; repeat++)); do
        run_operation "$operation" "$requests" "$repeat"
    done
done

echo
echo "============================================================"
echo "SUMMARY"
echo "============================================================"
for operation in read update insert delete; do
    echo
    echo "===== $operation ====="
    for ((repeat=1; repeat<=REPEATS; repeat++)); do
        grep -E '^(mode=|target=|wall_seconds=|latency_success_only_us:|database_call_us:|db_queue_wait_us:)' \
            "$OUTDIR/${operation}_run${repeat}.log" || true
    done
done

echo
echo "CSV: $CSV"
echo "Logs: $OUTDIR"
