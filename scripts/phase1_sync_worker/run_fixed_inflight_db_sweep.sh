#!/usr/bin/env bash

# 用途：在固定的高并发条件下，分别测量 READ、UPDATE、INSERT、DELETE 的性能，并重复测试以便比较结果。
# 固定客户端配置：4 个 Worker、每个连接 Window=512，总在途 RPC 请求数为 2,048。
# 默认服务端配置：4 个 IO 线程、16 个 DB Worker；服务端 CPU 绑核为 0-3，客户端为 4-7。
# 测试顺序及每轮请求数量：
# 1. READ：每轮 30,000,000 次，重复 3 轮。
# 2. UPDATE：每轮 3,000,000 次，重复 3 轮。
# 3. INSERT：每轮 1,000,000 次，重复 3 轮。
# 4. DELETE：每轮 1,000,000 次，重复 3 轮。
# 每种操作的一轮测试只启动一个客户端进程，该进程只发送当前指定类型的请求。
# 每轮开始前先停止上一次启动的服务端、重置四张压测表，再启动一个新的服务端和客户端。
# 每轮结束后关闭本轮服务端并等待 3 秒；四种操作各 3 轮，因此总共运行 12 次客户端进程。
# 测试结果、每轮日志和环境配置保存在 ~/rpc-bench/ 下；脚本要求 RPC 端口 9000 开始时未被占用。

set -Eeuo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
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
DB_PORT="${DB_PORT:-5432}"
DB_USER="${DB_USER:-ordersystem_bench}"
DB_NAME="${DB_NAME:-ordersystem_bench}"
if [[ -z "${DB_PASSWORD+x}" ]]; then
    read -rsp "PostgreSQL password for ${DB_USER}: " DB_PASSWORD
    echo
fi
export DB_HOST DB_PORT DB_USER DB_PASSWORD DB_NAME

for executable in "$SERVER" "$CLIENT"; do
    if [[ ! -x "$executable" ]]; then
        echo "Executable not found: $executable" >&2
        echo "Build OrderSystem first: cmake --preset linux-release && cmake --build --preset build-linux-release" >&2
        exit 2
    fi
done

if ! command -v psql >/dev/null 2>&1; then
    echo "psql client not found. Install the PostgreSQL client first." >&2
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
        "$ROOT/scripts/common/reset_benchmark_tables.sh" \
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
