#pragma once

#include "nebula/net/event_loop.h"

#include <libpq-fe.h>

#include <array>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ordersystem::benchmark
{

enum class DbOperation
{
    Read,
    Insert,
    Update,
    Delete
};

// key 对应记录 ID，value 只在 Insert 时使用。
struct DbRequest
{
    DbOperation operation{};
    std::uint64_t key{0};
    std::uint64_t value{0};
};

struct DbResult
{
    bool success{false};
    std::uint64_t value{0};
    std::string error;

    std::uint64_t queue_wait_us{0};                                                 // 请求在 DB Worker 队列中等待的时间，单位：微秒
    std::uint64_t database_call_us{0};                                              // 执行 SQL 到拿到数据库结果的时间，单位：微秒
};

struct DbConfig
{
    std::string host{"127.0.0.1"};
    std::uint16_t port{5432};
    std::string user{"ordersystem_bench"};
    std::string password;
    std::string database{"ordersystem_bench"};
};

// 每条连接提前准备四条 SQL，后续执行时只需要传入参数。
// 当前使用同步接口，因此一条连接只能由一个 Worker 使用。
class DbConnection final
{
public:
    explicit DbConnection(const DbConfig& config);
    ~DbConnection();

    DbConnection(const DbConnection&) = delete;
    DbConnection& operator=(const DbConnection&) = delete;

    DbResult Execute(const DbRequest& request);                                     // 执行同步 SQL 并等待结果，不能在 EventLoop 线程调用。

private:
    // 在当前连接上创建预处理语句，后续通过名称重复执行。
    void Prepare(std::size_t index, const char* name, const char* sql, int parameter_count);

    void Close() noexcept;

    [[nodiscard]] std::string ConnectionError() const;

    PGconn* connection_{nullptr};
    std::array<const char*, 4> statement_names_{};                                  // 四条预处理语句的名称，位置与 DbOperation 对应。
};

// 每个 Worker 独占一条 PostgreSQL 连接。
// 当前阶段通过 Worker 执行同步 SQL，避免阻塞 RPC EventLoop。
class DbPool final
{
public:
    using Job = std::function<void(DbConnection&)>;                                 // 数据库任务回调，接收 Worker 的连接并执行操作。

    DbPool(DbConfig config, std::size_t worker_count);                              // 等待所有 Worker 完成连接初始化，失败时抛出异常。
    ~DbPool();

    DbPool(const DbPool&) = delete;
    DbPool& operator=(const DbPool&) = delete;

    void Submit(Job job);                                                           // 将数据库任务放入队列，由空闲 Worker 执行。

    [[nodiscard]] std::size_t WorkerCount() const noexcept;

private:
    void WorkerMain();                                                              // Worker 线程入口：建立数据库连接，然后循环执行队列中的任务。

    DbConfig config_;
    std::size_t worker_count_;
    std::vector<std::thread> workers_;

    std::mutex queue_mutex_;                                                        // 保护任务队列和关闭状态
    std::condition_variable queue_ready_;                                           // 队列为空时让 Worker 等待，有新任务或关闭时唤醒
    std::deque<Job> jobs_;
    bool stopping_{false};                                                          // 置位后不再接收新任务，Worker 处理完剩余任务后退出

    std::mutex startup_mutex_;
    std::condition_variable startup_ready_;                                         // 所有 Worker 报告连接初始化结果后通知构造线程
    std::size_t startup_finished_{0};                                               // 已完成连接初始化检查的 Worker 数量
    std::string startup_error_;                                                     // 保存第一个数据库连接初始化失败的原因
};

// 协程在这里提交数据库任务并挂起。
// Worker 完成 SQL 后，将协程恢复任务投回原来的 EventLoop。
class DbOperationAwaiter final
{
public:
    DbOperationAwaiter(DbPool& pool, nebula::net::EventLoop* loop, DbRequest request);

    [[nodiscard]] bool await_ready() const noexcept;                                // 返回 false，让协程进入 await_suspend。

    // 提交数据库任务并挂起协程，完成后投回所属 EventLoop 恢复。
    void await_suspend(std::coroutine_handle<> continuation);

    DbResult await_resume() noexcept;

private:
    DbPool& pool_;
    nebula::net::EventLoop* loop_;
    DbRequest request_;
    DbResult result_;
};

}  // namespace ordersystem::benchmark
