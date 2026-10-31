#!/usr/bin/env bash
set -Eeuo pipefail

DB_HOST="${DB_HOST:-127.0.0.1}"
DB_PORT="${DB_PORT:-5432}"
DB_USER="${DB_USER:-ordersystem_bench}"
DB_NAME="${DB_NAME:-ordersystem_bench}"

if [[ -z "${DB_PASSWORD+x}" ]]; then
    read -rsp "PostgreSQL password for ${DB_USER}: " DB_PASSWORD
    echo
fi

export PGPASSWORD="$DB_PASSWORD"
psql \
    --no-psqlrc \
    --set ON_ERROR_STOP=1 \
    --host="$DB_HOST" \
    --port="$DB_PORT" \
    --username="$DB_USER" \
    --dbname="$DB_NAME" <<'SQL'
TRUNCATE TABLE bench_insert_records, bench_delete_records;
INSERT INTO bench_delete_records (id, value)
SELECT id, value FROM bench_read_records;
UPDATE bench_update_records SET value = id % 100000;
SQL

unset PGPASSWORD
echo "PostgreSQL benchmark tables reset."
