# OrderSystem

OrderSystem is a small RPC benchmark that measures PostgreSQL operations on a real RPC request path.

## Current experiment

- Reuse the existing NebulaRPC coroutine RPC framework.
- Keep the fixed client model: 4 workers and `window=512` per connection (2048 in-flight RPCs in total).
- Compare primary-key reads, inserts, updates, and deletes in PostgreSQL.
- Record QPS, average latency, P50, P95, P99, and P99.9.

## Database access in phase 1 (finished)

Phase 1 measured the cost of moving blocking PostgreSQL calls away from the RPC EventLoop:

- PostgreSQL uses `libpq` prepared statements.
- A fixed-size DB Worker pool owns one PostgreSQL connection per worker.
- A coroutine queues a job, a worker runs the blocking SQL, and the result is posted back to the original EventLoop.
- Every request crosses threads on the way to the worker and again on the way back.

The phase 1 source remains in the repository for comparison. The current benchmark server no longer builds or uses the worker pool.

## Database access in phase 2 (finished)

Phase 2 removed the DB Worker hand-off and gave each IO EventLoop its own PostgreSQL connection:

- Each IO EventLoop owns exactly one PostgreSQL connection.
- The PostgreSQL socket is registered on that same EventLoop through `nebula::net::Channel`.
- A coroutine submits a request and suspends; the socket callbacks drive nonblocking send/receive, then the same EventLoop resumes the coroutine.
- There is no DB Worker thread pool. `io_threads=4` means four IO EventLoops and four PostgreSQL connections.

Phase 2 allowed only one SQL request in flight per connection. Other requests waited in that EventLoop's queue. That queueing time was reported as `db_queue_wait_us`.

## Database access in phase 3 (current)

Phase 3 keeps the phase 2 ownership model but enables PostgreSQL Pipeline Mode:

- Each IO EventLoop still owns one PostgreSQL connection.
- Every prepared SQL request is sent as soon as it arrives; there is no fixed batch size to fill first.
- `PQsendQueryPrepared()` queues each request immediately. At the end of the current loop turn a single `PQpipelineSync()` closes the round, and `PQflush()` sends the buffered data without blocking the EventLoop.
- Requests submitted in the same loop turn share that one synchronization point; no empty sync is sent when nothing was queued.
- The PostgreSQL socket remains registered on the same EventLoop. `PQconsumeInput()` and `PQgetResult()` process results in send order.
- Two FIFO queues keep the mapping explicit: one holds requests sent but not yet answered, the other holds answered requests waiting for the sync point.
- A request is not reported complete until its `PGRES_PIPELINE_SYNC` result arrives.

The server reads these settings from the environment:

- `DB_MAX_OUTSTANDING_REQUESTS`: maximum number of in-flight plus completed-but-unconfirmed requests per EventLoop; defaults to `4096`.

### Pipeline error behavior

A pipeline synchronization point also marks the end of PostgreSQL's implicit transaction for that batch. If one SQL statement fails, PostgreSQL aborts the rest of that batch; writes that had already run in the same batch are rolled back. The server therefore waits for the sync result before completing any RPC in that batch. If the batch failed, earlier successful-looking results are also reported as failed because their writes were rolled back. This avoids telling the client that a write succeeded when PostgreSQL rolled it back.

Pipeline Mode does not execute statements in parallel on one connection. It reduces idle network round trips by allowing multiple statements to be sent before their results are collected.

## Database migration status

The project uses PostgreSQL only. MySQL and MariaDB client headers, libraries, SQL syntax, and command-line tools have been removed.

See `scripts/env/setup_ubuntu_env.sh`, `sql/schema.sql`, and `sql/seed.sql` for installation and database setup.

### Phase 3 test scripts

- `scripts/phase3_pipeline/run_phase3_pipeline_smoke_test.sh`: run a small READ/INSERT/UPDATE/DELETE correctness check.
- `scripts/phase3_pipeline/run_phase3_pipeline_high_concurrency_test.sh`: run the four operations with the fixed high-concurrency client workload.
- `scripts/windows/sync_phase3_results.ps1` (or the `.bat` wrapper): copy the latest Linux results into `perf-results/phase3_pipeline/` on Windows.

The test scripts reset the dedicated benchmark tables before each operation. Run them only against the disposable `ordersystem_bench` database.
