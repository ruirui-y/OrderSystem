# OrderSystem

OrderSystem is a small RPC benchmark that measures the cost of PostgreSQL operations on a real request path.

## Current experiment

- Reuse the existing NebulaRPC coroutine RPC framework.
- Keep the current fixed client model: 4 workers and `window=512` per connection (2048 in-flight RPCs in total).
- Compare primary-key reads, inserts, updates, and deletes in PostgreSQL.
- Record QPS, average latency, P50, P95, P99, and P99.9.

## Database access in phase 1 (finished)

Phase 1 measured what a blocking PostgreSQL call costs when it is moved off the RPC EventLoop:

- PostgreSQL through `libpq` prepared statements.
- A fixed-size database worker pool (`db_pool.h`, `db_pool.cpp`), one PostgreSQL connection per worker.
- A coroutine queues a job, a worker runs the blocking SQL, and the completion is posted back to the originating EventLoop.
- Every SQL still costs one thread hand-off in each direction. Removing that hand-off is what phase 2 is for.

The phase 1 source stays in the repository for reference. The benchmark binaries now build the phase 2 database context instead of the worker pool.

## Database access in phase 2 (current)

Phase 2 drops the worker pool and lets every IO EventLoop drive its own PostgreSQL connection:

- Each IO EventLoop owns exactly one PostgreSQL connection (`loop_database_context.h`, `loop_database_context.cpp`).
- The connection socket is registered on that same EventLoop through `nebula::net::Channel`, so the loop reacts to readable and writable events instead of blocking.
- A coroutine awaits `DbOperationAwaiter`; the request is queued on the loop's connection and the coroutine suspends. The same EventLoop resumes it once the result arrives.
- This phase creates no database worker threads: `io_threads=4` means four connections and zero workers.

Limits that still apply in phase 2:

- One SQL in flight per connection. A second request waits in that loop's pending queue until the active statement finishes, which shows up as `db_queue_wait_us`.
- PostgreSQL Pipeline Mode is **not** enabled yet. Sending several statements on one connection before reading their results is phase 3.

## Database migration status

The project now uses PostgreSQL only. MySQL and MariaDB client headers, libraries, SQL syntax, and command-line tools have been removed.

See `scripts/env/setup_ubuntu_env.sh`, `sql/schema.sql`, and `sql/seed.sql` for installation and database setup.
