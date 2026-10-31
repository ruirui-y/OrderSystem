#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/build/linux-release/bin"
SERVER="$BIN/order_db_benchmark_server"
CLIENT="$BIN/order_db_benchmark_client"
RESET_SCRIPT="$ROOT/scripts/reset_benchmark_tables.sh"

RPC_HOST="${RPC_HOST:-127.0.0.1}"
RPC_PORT="${RPC_PORT:-9000}"
DB_HOST="${DB_HOST:-127.0.0.1}"
DB_PORT="${DB_PORT:-5432}"
DB_USER="${DB_USER:-ordersystem_bench}"
DB_NAME="${DB_NAME:-ordersystem_bench}"

WORKERS="${HIGH_WORKERS:-4}"
WINDOW="${HIGH_WINDOW:-512}"
IO_THREADS="${HIGH_IO_THREADS:-4}"
DB_WORKERS="${HIGH_DB_WORKERS:-16}"
REPEATS="${HIGH_REPEATS:-1}"
REQUESTS_READ="${HIGH_REQUESTS_READ:-1000000}"
REQUESTS_UPDATE="${HIGH_REQUESTS_UPDATE:-200000}"
REQUESTS_INSERT="${HIGH_REQUESTS_INSERT:-200000}"
REQUESTS_DELETE="${HIGH_REQUESTS_DELETE:-200000}"
TIMEOUT_MS="${HIGH_TIMEOUT_MS:-10000}"
CONNECT_TIMEOUT_MS="${HIGH_CONNECT_TIMEOUT_MS:-10000}"
MAX_SECONDS="${HIGH_MAX_SECONDS:-600}"
COOLDOWN_SECONDS="${HIGH_COOLDOWN_SECONDS:-3}"

SERVER_CPU="${HIGH_SERVER_CPU:-0-3}"
CLIENT_CPU="${HIGH_CLIENT_CPU:-4-7}"
PIN_CPUS="${HIGH_PIN_CPUS:-1}"

fail()
{
    echo "ERROR: $*" >&2
    exit 1
}

check_number()
{
    local name="$1"
    local value="$2"
    local minimum="$3"
    local maximum="$4"
    local number=0

    [[ "$value" =~ ^[0-9]+$ ]] || fail "$name must be an integer"
    number=$((10#$value))
    (( number >= minimum && number <= maximum )) || fail "$name must be between $minimum and $maximum"
}

rpc_server_ready()
{
    bash -c 'exec 3<>/dev/tcp/$1/$2' _ "$RPC_HOST" "$RPC_PORT" 2>/dev/null
}

query_count()
{
    psql --no-psqlrc --set ON_ERROR_STOP=1 \
        --host="$DB_HOST" --port="$DB_PORT" \
        --username="$DB_USER" --dbname="$DB_NAME" \
        --tuples-only --no-align --command="$1"
}

SERVER_PID=""
SERVER_OWNED=0

stop_owned_server()
{
    if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
        echo "Stopping RPC server started by this script (PID=$SERVER_PID)..."
        kill -TERM "$SERVER_PID" 2>/dev/null || true
        for _ in {1..100}; do
            kill -0 "$SERVER_PID" 2>/dev/null || break
            sleep 0.1
        done
        if kill -0 "$SERVER_PID" 2>/dev/null; then
            kill -KILL "$SERVER_PID" 2>/dev/null || true
        fi
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    SERVER_PID=""
    SERVER_OWNED=0
}

cleanup()
{
    stop_owned_server
    unset PGPASSWORD
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

start_server()
{
    local server_log="$1"

    if rpc_server_ready; then
        echo "Using existing RPC server at ${RPC_HOST}:${RPC_PORT}; this script will not stop it."
        SERVER_OWNED=0
        SERVER_PID=""
        return
    fi

    if ss -ltnH "sport = :$RPC_PORT" 2>/dev/null | grep -q .; then
        fail "port $RPC_PORT is occupied, but it is not accepting an RPC connection"
    fi

    echo "RPC server is not running; starting it automatically..."
    if [[ "$PIN_CPUS" == "1" ]]; then
        DB_HOST="$DB_HOST" DB_PORT="$DB_PORT" DB_USER="$DB_USER" DB_PASSWORD="$DB_PASSWORD" DB_NAME="$DB_NAME" \
            taskset -c "$SERVER_CPU" "$SERVER" "$RPC_PORT" "$IO_THREADS" "$DB_WORKERS" >"$server_log" 2>&1 &
    else
        DB_HOST="$DB_HOST" DB_PORT="$DB_PORT" DB_USER="$DB_USER" DB_PASSWORD="$DB_PASSWORD" DB_NAME="$DB_NAME" \
            "$SERVER" "$RPC_PORT" "$IO_THREADS" "$DB_WORKERS" >"$server_log" 2>&1 &
    fi
    SERVER_PID=$!
    SERVER_OWNED=1

    for _ in {1..150}; do
        if rpc_server_ready; then
            echo "RPC server started (PID=$SERVER_PID)."
            return
        fi
        if ! kill -0 "$SERVER_PID" 2>/dev/null; then
            cat "$server_log" >&2
            fail "RPC server exited during startup; see $server_log"
        fi
        sleep 0.1
    done

    cat "$server_log" >&2
    fail "RPC server did not become ready at ${RPC_HOST}:${RPC_PORT}; see $server_log"
}

for executable in "$SERVER" "$CLIENT" "$RESET_SCRIPT"; do
    [[ -f "$executable" ]] || fail "required file not found: $executable"
done
[[ -x "$SERVER" ]] || fail "server is not executable: $SERVER"
[[ -x "$CLIENT" ]] || fail "client is not executable: $CLIENT"
command -v psql >/dev/null 2>&1 || fail "psql was not found; install the PostgreSQL client first"
command -v ss >/dev/null 2>&1 || fail "ss was not found; install iproute2 first"

check_number HIGH_WORKERS "$WORKERS" 1 128
check_number HIGH_WINDOW "$WINDOW" 1 65536
check_number HIGH_IO_THREADS "$IO_THREADS" 1 128
check_number RPC_PORT "$RPC_PORT" 1 65535
check_number DB_PORT "$DB_PORT" 1 65535
check_number HIGH_DB_WORKERS "$DB_WORKERS" 1 512
check_number HIGH_REPEATS "$REPEATS" 1 10
check_number HIGH_REQUESTS_READ "$REQUESTS_READ" "$WORKERS" 100000000
check_number HIGH_REQUESTS_UPDATE "$REQUESTS_UPDATE" "$WORKERS" 100000000
check_number HIGH_REQUESTS_INSERT "$REQUESTS_INSERT" "$WORKERS" 100000000
check_number HIGH_REQUESTS_DELETE "$REQUESTS_DELETE" "$WORKERS" 1000000
check_number HIGH_TIMEOUT_MS "$TIMEOUT_MS" 1 120000
check_number HIGH_CONNECT_TIMEOUT_MS "$CONNECT_TIMEOUT_MS" 1 120000
check_number HIGH_MAX_SECONDS "$MAX_SECONDS" 1 86400
check_number HIGH_COOLDOWN_SECONDS "$COOLDOWN_SECONDS" 0 60

if [[ "$DB_NAME" != "ordersystem_bench" && "${ALLOW_NON_BENCH_DB:-0}" != "1" ]]; then
    fail "this script resets benchmark tables; set ALLOW_NON_BENCH_DB=1 only if $DB_NAME is a disposable benchmark database"
fi

if [[ "$PIN_CPUS" == "1" ]]; then
    if ! command -v taskset >/dev/null 2>&1 || \
       ! taskset -c "$SERVER_CPU" true >/dev/null 2>&1 || \
       ! taskset -c "$CLIENT_CPU" true >/dev/null 2>&1; then
        echo "WARNING: CPU affinity is unavailable for $SERVER_CPU/$CLIENT_CPU; running without taskset."
        PIN_CPUS=0
    fi
fi

if [[ -z "${DB_PASSWORD:-}" ]]; then
    read -rsp "PostgreSQL password for ${DB_USER}: " DB_PASSWORD
    echo
fi
export DB_HOST DB_PORT DB_USER DB_NAME DB_PASSWORD
export PGPASSWORD="$DB_PASSWORD"

psql --no-psqlrc --set ON_ERROR_STOP=1 \
    --host="$DB_HOST" --port="$DB_PORT" \
    --username="$DB_USER" --dbname="$DB_NAME" \
    --tuples-only --no-align --command="SELECT 1" >/dev/null \
    || fail "cannot connect to PostgreSQL database $DB_NAME as $DB_USER"

read_rows="$(query_count "SELECT COUNT(*) FROM bench_read_records")"
update_rows="$(query_count "SELECT COUNT(*) FROM bench_update_records")"
[[ "$read_rows" == "1000000" ]] || fail "bench_read_records has $read_rows rows; expected 1000000. Initialize sql/seed.sql first."
[[ "$update_rows" == "1000000" ]] || fail "bench_update_records has $update_rows rows; expected 1000000. Initialize sql/seed.sql first."

STAMP="$(date +%Y%m%d_%H%M%S)"
OUTDIR="${HIGH_OUTPUT_DIR:-$HOME/rpc-bench/ordersystem_high_concurrency_${STAMP}}"
mkdir -p "$OUTDIR"
CSV="$OUTDIR/results.csv"

cat > "$OUTDIR/environment.txt" <<META
created_at=$(date -Is)
database=$DB_NAME
database_host=$DB_HOST
database_port=$DB_PORT
rpc_host=$RPC_HOST
rpc_port=$RPC_PORT
workers=$WORKERS
window=$WINDOW
total_inflight=$((WORKERS * WINDOW))
io_threads=$IO_THREADS
db_workers=$DB_WORKERS
repeats=$REPEATS
requests_read=$REQUESTS_READ
requests_update=$REQUESTS_UPDATE
requests_insert=$REQUESTS_INSERT
requests_delete=$REQUESTS_DELETE
server_cpu=$SERVER_CPU
client_cpu=$CLIENT_CPU
cpu_affinity=$PIN_CPUS
timeout_ms=$TIMEOUT_MS
max_seconds=$MAX_SECONDS
META

run_client()
{
    local operation="$1"
    local requests="$2"
    local repeat="$3"
    local client_log="$OUTDIR/${operation}_run${repeat}.log"

    if [[ "$PIN_CPUS" == "1" ]]; then
        taskset -c "$CLIENT_CPU" "$CLIENT" \
            --host "$RPC_HOST" --port "$RPC_PORT" \
            --workers "$WORKERS" --window "$WINDOW" \
            --requests "$requests" --operation "$operation" \
            --timeout-ms "$TIMEOUT_MS" \
            --connect-timeout-ms "$CONNECT_TIMEOUT_MS" \
            --max-seconds "$MAX_SECONDS" \
            --csv "$CSV" >"$client_log" 2>&1 || {
                cat "$client_log"
                fail "$operation run $repeat failed; see $client_log"
            }
    else
        "$CLIENT" \
            --host "$RPC_HOST" --port "$RPC_PORT" \
            --workers "$WORKERS" --window "$WINDOW" \
            --requests "$requests" --operation "$operation" \
            --timeout-ms "$TIMEOUT_MS" \
            --connect-timeout-ms "$CONNECT_TIMEOUT_MS" \
            --max-seconds "$MAX_SECONDS" \
            --csv "$CSV" >"$client_log" 2>&1 || {
                cat "$client_log"
                fail "$operation run $repeat failed; see $client_log"
            }
    fi

    cat "$client_log"
}

verify_operation()
{
    local operation="$1"
    local requests="$2"
    local actual=""
    local expected=""
    local table=""

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
    esac

    actual="$(query_count "SELECT COUNT(*) FROM $table")"
    [[ "$actual" == "$expected" ]] || fail "$operation finished but $table has $actual rows; expected $expected"
    echo "PASS: $operation, $table row count=$actual"
}

run_operation()
{
    local operation="$1"
    local requests="$2"

    for ((repeat = 1; repeat <= REPEATS; ++repeat)); do
        echo
        echo "============================================================"
        echo "operation=$operation requests=$requests repeat=$repeat/$REPEATS"
        echo "workers=$WORKERS window=$WINDOW total_inflight=$((WORKERS * WINDOW))"
        echo "============================================================"

        bash "$RESET_SCRIPT" >"$OUTDIR/reset_${operation}_run${repeat}.log" 2>&1 || {
            cat "$OUTDIR/reset_${operation}_run${repeat}.log" >&2
            fail "failed to reset benchmark tables before $operation run $repeat"
        }

        start_server "$OUTDIR/server_${operation}_run${repeat}.log"
        run_client "$operation" "$requests" "$repeat"
        verify_operation "$operation" "$requests"

        stop_owned_server
        sleep "$COOLDOWN_SECONDS"
    done
}

echo "============================================================"
echo "OrderSystem PostgreSQL high-concurrency CRUD test"
echo "database=$DB_NAME at $DB_HOST:$DB_PORT"
echo "RPC server=$RPC_HOST:$RPC_PORT"
echo "server config: io_threads=$IO_THREADS, db_workers=$DB_WORKERS"
echo "client config: workers=$WORKERS, window=$WINDOW, total_inflight=$((WORKERS * WINDOW))"
echo "requests: read=$REQUESTS_READ update=$REQUESTS_UPDATE insert=$REQUESTS_INSERT delete=$REQUESTS_DELETE"
echo "repeats=$REPEATS cpu_affinity=$PIN_CPUS"
echo "output=$OUTDIR"
echo "============================================================"

run_operation read "$REQUESTS_READ"
run_operation update "$REQUESTS_UPDATE"
run_operation insert "$REQUESTS_INSERT"
run_operation delete "$REQUESTS_DELETE"

stop_owned_server

echo
echo "============================================================"
echo "ALL HIGH-CONCURRENCY CRUD TESTS PASSED"
echo "CSV: $CSV"
echo "Logs: $OUTDIR"
echo "============================================================"
