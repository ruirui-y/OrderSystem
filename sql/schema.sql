CREATE TABLE IF NOT EXISTS bench_read_records
(
    id BIGINT PRIMARY KEY CHECK (id > 0),
    value BIGINT NOT NULL CHECK (value >= 0)
);

CREATE TABLE IF NOT EXISTS bench_update_records
(
    id BIGINT PRIMARY KEY CHECK (id > 0),
    value BIGINT NOT NULL CHECK (value >= 0)
);

CREATE TABLE IF NOT EXISTS bench_delete_records
(
    id BIGINT PRIMARY KEY CHECK (id > 0),
    value BIGINT NOT NULL CHECK (value >= 0)
);

CREATE TABLE IF NOT EXISTS bench_insert_records
(
    id BIGINT PRIMARY KEY CHECK (id > 0),
    value BIGINT NOT NULL CHECK (value >= 0)
);
