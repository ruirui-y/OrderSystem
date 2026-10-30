# OrderSystem

OrderSystem 的第一阶段重点不是实现完整的电商功能，而是用一个最小的 RPC 服务把 MySQL 操作放进真实请求路径，测量固定并发下读写操作对吞吐和延迟的影响。

## 当前实验

- 复用现有 NebulaRPC 协程 RPC 框架。
- 固定客户端为 4 个 Worker、每条连接 `window=512`，总在途 RPC 为 2048。
- 比较按主键读取、插入、更新、删除四种 MySQL 操作。
- 每种操作记录 QPS、平均延迟、P50、P95、P99 和 P99.9。
- 使用固定大小的数据库工作线程池，避免阻塞式 MySQL 调用直接卡住 RPC EventLoop。

详细步骤见 [`docs/database-operation-benchmark.md`](docs/database-operation-benchmark.md)。


## 第一阶段：数据库读写性能实验

第一阶段先不实现完整订单业务，而是固定 NebulaRPC 的并发模型，单独比较 MySQL 主键读取、插入、更新和删除的吞吐与延迟。测试结果同时记录 RPC 完整耗时、数据库线程池排队时间和数据库调用耗时。具体运行方式见 `docs/database-operation-benchmark.md`。
