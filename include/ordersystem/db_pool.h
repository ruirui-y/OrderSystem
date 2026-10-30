#pragma once

#include "nebula/net/event_loop.h"

#include <mysql.h>

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
    std::uint64_t queue_wait_us{0};
    std::uint64_t database_call_us{0};
};

struct DbConfig
{
    std::string host{"127.0.0.1"};
    std::uint16_t port{3306};
    std::string user{"ordersystem_bench"};
    std::string password;
    std::string database{"ordersystem_bench"};
};

class DbConnection final
{
public:
    explicit DbConnection(const DbConfig& config);
    ~DbConnection();

    DbConnection(const DbConnection&) = delete;
    DbConnection& operator=(const DbConnection&) = delete;

    DbResult Execute(const DbRequest& request);

private:
    void Prepare(std::size_t index, const char* sql);
    void Close() noexcept;
    [[nodiscard]] std::string StatementError(MYSQL_STMT* statement) const;

    MYSQL* connection_{nullptr};
    std::array<MYSQL_STMT*, 4> statements_{};
    bool mysql_thread_initialized_{false};
};

class DbPool final
{
public:
    using Job = std::function<void(DbConnection&)>;

    DbPool(DbConfig config, std::size_t worker_count);
    ~DbPool();

    DbPool(const DbPool&) = delete;
    DbPool& operator=(const DbPool&) = delete;

    void Submit(Job job);
    [[nodiscard]] std::size_t WorkerCount() const noexcept;

private:
    void WorkerMain();

    DbConfig config_;
    std::size_t worker_count_;
    std::vector<std::thread> workers_;

    std::mutex queue_mutex_;
    std::condition_variable queue_ready_;
    std::deque<Job> jobs_;
    bool stopping_{false};

    std::mutex startup_mutex_;
    std::condition_variable startup_ready_;
    std::size_t startup_finished_{0};
    std::string startup_error_;
};

// 把阻塞的 MySQL 操作交给数据库工作线程，完成后再回到原来的 EventLoop 继续 RPC。
class DbOperationAwaiter final
{
public:
    DbOperationAwaiter(DbPool& pool, nebula::net::EventLoop* loop, DbRequest request);

    [[nodiscard]] bool await_ready() const noexcept;
    void await_suspend(std::coroutine_handle<> continuation);
    DbResult await_resume() noexcept;

private:
    DbPool& pool_;
    nebula::net::EventLoop* loop_;
    DbRequest request_;
    DbResult result_;
};

}  // namespace ordersystem::benchmark
