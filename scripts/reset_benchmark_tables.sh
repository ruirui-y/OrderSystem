#!/usr/bin/env bash

# 用途：把四张 PostgreSQL 压测表恢复到下一轮测试需要的状态。
# 执行后：
# 1. 清空 bench_insert_records，确保下一轮 INSERT 从空表开始。
# 2. 根据 bench_read_records 重新填充 bench_delete_records，恢复 100 万条待删除记录。
# 3. 将 bench_update_records 的 value 恢复为 id % 100000。
# 4. bench_read_records 作为基准数据表保持不变。
# 注意：本脚本会删除并重建压测表中的测试数据，只能对专用压测数据库执行。

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
