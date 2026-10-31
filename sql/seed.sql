TRUNCATE TABLE
    bench_read_records,
    bench_update_records,
    bench_delete_records,
    bench_insert_records;

-- Generate one million primary keys for read, update, and delete tests.
INSERT INTO bench_read_records (id, value)
SELECT id, id % 100000
FROM generate_series(1, 1000000) AS numbers(id);

INSERT INTO bench_update_records (id, value)
SELECT id, value FROM bench_read_records;

INSERT INTO bench_delete_records (id, value)
SELECT id, value FROM bench_read_records;

SELECT 'read' AS table_name, COUNT(*) AS total FROM bench_read_records
UNION ALL
SELECT 'update', COUNT(*) FROM bench_update_records
UNION ALL
SELECT 'delete', COUNT(*) FROM bench_delete_records
UNION ALL
SELECT 'insert', COUNT(*) FROM bench_insert_records;
