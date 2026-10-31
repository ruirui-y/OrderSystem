#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/build/linux-release/bin"
SERVER_HOST="${RPC_HOST:-127.0.0.1}"
SERVER_PORT="${RPC_PORT:-9000}"
IO_THREADS="${SMOKE_IO_THREADS:-4}"
DB_WORKERS="${SMOKE_DB_WORKERS:-16}"
WORKERS="${SMOKE_WORKERS:-4}"
WINDOW="${SMOKE_WINDOW:-8}"
REQUESTS="${SMOKE_REQUESTS:-1000}"
TIMEOUT_MS="${SMOKE_TIMEOUT_MS:-10000}"
CONNECT_TIMEOUT_MS="${SMOKE_CONNECT_TIMEOUT_MS:-10000}"
MAX_SECONDS="${SMOKE_MAX_SECONDS:-120}"

DB_HOST="${DB_HOST:-127.0.0.1}"
DB_PORT="${DB_PORT:-5432}"
DB_USER="${DB_USER:-ordersystem_bench}"
DB_NAME="${DB_NAME:-ordersystem_bench}"

SERVER="$BIN/order_db_benchmark_server"
CLIENT="$BIN/order_db_benchmark_client"
RESET_SCRIPT="$ROOT/scripts/reset_benchmark_tables.sh"

fail()
{
    echo "ERROR: $*" >&2
    exit 1
}

rpc_server_ready()
{
    bash -c 'exec 3<>/dev/tcp/$1/$2' _ "$SERVER_HOST" "$SERVER_PORT" 2>/dev/null
}

for executable in "$SERVER" "$CLIENT" "$RESET_SCRIPT"; do
    [[ -f "$executable" ]] || fail "required file not found: $executable"
done
[[ -x "$SERVER" ]] || fail "server is not executable: $SERVER"
[[ -x "$CLIENT" ]] || fail "client is not executable: $CLIENT"
command -v psql >/dev/null 2>&1 || fail "psql was not found; install the PostgreSQL client first"

[[ "$IO_THREADS" =~ ^[0-9]+$ ]] && (( IO_THREADS > 0 && IO_THREADS <= 128 )) || fail "SMOKE_IO_THREADS must be between 1 and 128"
[[ "$DB_WORKERS" =~ ^[0-9]+$ ]] && (( DB_WORKERS > 0 && DB_WORKERS <= 512 )) || fail "SMOKE_DB_WORKERS must be between 1 and 512"
[[ "$WORKERS" =~ ^[0-9]+$ ]] && (( WORKERS > 0 && WORKERS <= 128 )) || fail "SMOKE_WORKERS must be between 1 and 128"
[[ "$WINDOW" =~ ^[0-9]+$ ]] && (( WINDOW > 0 && WINDOW <= 65536 )) || fail "SMOKE_WINDOW must be between 1 and 65536"
[[ "$REQUESTS" =~ ^[0-9]+$ ]] && (( REQUESTS >= WORKERS && REQUESTS <= 1000000 )) || fail "SMOKE_REQUESTS must be between WORKERS and 1000000"

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
    || fail "cannot connect to PostgreSQL database ${DB_NAME} as ${DB_USER}"

READ_ROWS="$(psql --no-psqlrc --set ON_ERROR_STOP=1 \
    --host="$DB_HOST" --port="$DB_PORT" \
    --username="$DB_USER" --dbname="$DB_NAME" \
    --tuples-only --no-align \
    --command="SELECT COUNT(*) FROM bench_read_records")"
[[ "$READ_ROWS" == "1000000" ]] || fail "bench_read_records contains ${READ_ROWS} rows; expected 1000000. Run sql/seed.sql first."

STAMP="$(date +%Y%m%d_%H%M%S)"
OUTDIR="${SMOKE_OUTPUT_DIR:-$HOME/rpc-bench/ordersystem_crud_smoke_${STAMP}}"
mkdir -p "$OUTDIR"
CSV="$OUTDIR/results.csv"
SERVER_PID=""

cleanup()
{
    if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
        echo
        echo "Stopping RPC server started by this script..."
        kill -TERM "$SERVER_PID" 2>/dev/null || true
        for _ in {1..50}; do
            kill -0 "$SERVER_PID" 2>/dev/null || break
            sleep 0.1
        done
        if kill -0 "$SERVER_PID" 2>/dev/null; then
            kill -KILL "$SERVER_PID" 2>/dev/null || true
        fi
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    unset PGPASSWORD
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

if rpc_server_ready; then
    echo "Using existing RPC server at ${SERVER_HOST}:${SERVER_PORT}."
else
    echo "RPC server is not running; starting it automatically..."
    "$SERVER" "$SERVER_PORT" "$IO_THREADS" "$DB_WORKERS" >"$OUTDIR/server.log" 2>&1 &
    SERVER_PID=$!

    for _ in {1..100}; do
        if rpc_server_ready; then
            echo "RPC server started (PID=${SERVER_PID})."
            break
        fi
        if ! kill -0 "$SERVER_PID" 2>/dev/null; then
            cat "$OUTDIR/server.log" >&2
            fail "RPC server exited during startup; see $OUTDIR/server.log"
        fi
        sleep 0.1
    done

    if ! rpc_server_ready; then
        cat "$OUTDIR/server.log" >&2
        fail "RPC server did not accept connections at ${SERVER_HOST}:${SERVER_PORT}; see $OUTDIR/server.log"
    fi
fi

query_count()
{
    psql --no-psqlrc --set ON_ERROR_STOP=1 \
        --host="$DB_HOST" --port="$DB_PORT" \
        --username="$DB_USER" --dbname="$DB_NAME" \
        --tuples-only --no-align --command="$1"
}

run_operation()
{
    local operation="$1"
    local log="$OUTDIR/${operation}.log"
    local reset_log="$OUTDIR/reset_${operation}.log"
    local actual=""
    local expected=""
    local table=""

    echo
    echo "============================================================"
    echo "Testing ${operation}: requests=${REQUESTS}, workers=${WORKERS}, window=${WINDOW}"
    echo "============================================================"

    bash "$RESET_SCRIPT" >"$reset_log" 2>&1 || {
        cat "$reset_log"
        fail "failed to reset benchmark tables before ${operation}"
    }

    "$CLIENT" \
        --host "$SERVER_HOST" \
        --port "$SERVER_PORT" \
        --workers "$WORKERS" \
        --window "$WINDOW" \
        --requests "$REQUESTS" \
        --operation "$operation" \
        --timeout-ms "$TIMEOUT_MS" \
        --connect-timeout-ms "$CONNECT_TIMEOUT_MS" \
        --max-seconds "$MAX_SECONDS" \
        --csv "$CSV" >"$log" 2>&1 || {
            cat "$log"
            fail "${operation} client test failed; see $log"
        }

    cat "$log"

    case "$operation" in
        read)
            table="bench_read_records"
            expected="1000000"
            ;;
        insert)
            table="bench_insert_records"
            expected="$REQUESTS"
            ;;
        update)
            table="bench_update_records"
            expected="1000000"
            ;;
        delete)
            table="bench_delete_records"
            expected="$((1000000 - REQUESTS))"
            ;;
    esac

    actual="$(query_count "SELECT COUNT(*) FROM ${table}")"
    [[ "$actual" == "$expected" ]] || fail "${operation} completed but ${table} has ${actual} rows; expected ${expected}"
    echo "PASS: ${operation}, ${table} row count=${actual}"
}

echo "OrderSystem PostgreSQL CRUD smoke test"
echo "database=${DB_NAME} at ${DB_HOST}:${DB_PORT}"
echo "RPC server=${SERVER_HOST}:${SERVER_PORT}"
echo "server config: io_threads=${IO_THREADS}, db_workers=${DB_WORKERS}"
echo "client config: workers=${WORKERS}, window=${WINDOW}, total_inflight=$((WORKERS * WINDOW)), requests_per_operation=${REQUESTS}"
echo "logs=${OUTDIR}"

run_operation read
run_operation insert
run_operation update
run_operation delete

echo
echo "============================================================"
echo "ALL CRUD SMOKE TESTS PASSED"
echo "CSV: ${CSV}"
echo "Logs: ${OUTDIR}"
echo "============================================================"
