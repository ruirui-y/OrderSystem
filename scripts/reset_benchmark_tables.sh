#!/usr/bin/env bash
set -Eeuo pipefail

DB_HOST="${DB_HOST:-127.0.0.1}"
DB_PORT="${DB_PORT:-3306}"
DB_USER="${DB_USER:-ordersystem_bench}"
DB_NAME="${DB_NAME:-ordersystem_bench}"

if [[ -z "${DB_PASSWORD+x}" ]]; then
    read -rsp "MySQL password for ${DB_USER}: " DB_PASSWORD
    echo
fi

MYSQL_PWD="$DB_PASSWORD" mysql \
    --protocol=TCP \
    --host="$DB_HOST" \
    --port="$DB_PORT" \
    --user="$DB_USER" \
    "$DB_NAME" <<'SQL'
TRUNCATE TABLE bench_insert_records;
TRUNCATE TABLE bench_delete_records;
INSERT INTO bench_delete_records (id, value)
SELECT id, value FROM bench_read_records;
UPDATE bench_update_records SET value = MOD(id, 100000);
SQL

echo "Benchmark tables reset."
