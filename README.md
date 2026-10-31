# OrderSystem

OrderSystem is a small RPC benchmark that measures the cost of PostgreSQL operations on a real request path.

## Current experiment

- Reuse the existing NebulaRPC coroutine RPC framework.
- Keep the current fixed client model: 4 workers and `window=512` per connection (2048 in-flight RPCs in total).
- Compare primary-key reads, inserts, updates, and deletes in PostgreSQL.
- Record QPS, average latency, P50, P95, P99, and P99.9.
- Phase 1 uses `libpq` prepared statements and the existing fixed-size database worker pool. Blocking PostgreSQL calls do not run on the RPC EventLoop.

## Database migration status

The project now uses PostgreSQL only. MySQL and MariaDB client headers, libraries, SQL syntax, and command-line tools have been removed.

This phase deliberately keeps the database worker pool so that the database migration can be tested independently from the later EventLoop and Pipeline Mode redesign.

See `scripts/setup_ubuntu_env.sh`, `sql/schema.sql`, and `sql/seed.sql` for installation and database setup.
