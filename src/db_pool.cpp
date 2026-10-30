#include "ordersystem/db_pool.h"

#include <chrono>
#include <exception>
#include <memory>
#include <stdexcept>
#include <utility>

namespace ordersystem::benchmark
{
namespace
{
constexpr std::size_t kReadStatement = 0;
constexpr std::size_t kInsertStatement = 1;
constexpr std::size_t kUpdateStatement = 2;
constexpr std::size_t kDeleteStatement = 3;

std::runtime_error MakeMysqlError(const std::string& action, const char* detail)
{
    return std::runtime_error(action + ": " + (detail == nullptr ? "unknown MySQL error" : detail));
}

void BindUnsignedValue(MYSQL_BIND& binding, unsigned long long* value)
{
    binding.buffer_type = MYSQL_TYPE_LONGLONG;
    binding.buffer = value;
    binding.is_unsigned = 1;
}
}  // namespace

DbConnection::DbConnection(const DbConfig& config)
{
    if (mysql_thread_init() != 0)
    {
        throw std::runtime_error("mysql_thread_init failed");
    }
    mysql_thread_initialized_ = true;

    try
    {
        connection_ = mysql_init(nullptr);
        if (connection_ == nullptr)
        {
            throw std::runtime_error("mysql_init failed");
        }

        unsigned int connect_timeout_seconds = 5;
        mysql_options(connection_, MYSQL_OPT_CONNECT_TIMEOUT, &connect_timeout_seconds);

        if (mysql_real_connect(connection_,
                              config.host.c_str(),
                              config.user.c_str(),
                              config.password.c_str(),
                              config.database.c_str(),
                              config.port,
                              nullptr,
                              0) == nullptr)
        {
            throw MakeMysqlError("connect to MySQL", mysql_error(connection_));
        }

        if (mysql_set_character_set(connection_, "utf8mb4") != 0)
        {
            throw MakeMysqlError("set MySQL character set", mysql_error(connection_));
        }

        // 每个 RPC 的写操作单独提交，方便直接比较单次读写的成本。
        if (mysql_autocommit(connection_, 1) != 0)
        {
            throw MakeMysqlError("enable MySQL autocommit", mysql_error(connection_));
        }

        Prepare(kReadStatement, "SELECT value FROM bench_read_records WHERE id = ?");
        Prepare(kInsertStatement, "INSERT INTO bench_insert_records (id, value) VALUES (?, ?)");
        Prepare(kUpdateStatement, "UPDATE bench_update_records SET value = value + 1 WHERE id = ?");
        Prepare(kDeleteStatement, "DELETE FROM bench_delete_records WHERE id = ?");
    }
    catch (...)
    {
        Close();
        throw;
    }
}

DbConnection::~DbConnection()
{
    Close();
}

void DbConnection::Prepare(std::size_t index, const char* sql)
{
    MYSQL_STMT* statement = mysql_stmt_init(connection_);
    if (statement == nullptr)
    {
        throw MakeMysqlError("create prepared statement", mysql_error(connection_));
    }

    if (mysql_stmt_prepare(statement, sql, static_cast<unsigned long>(std::char_traits<char>::length(sql))) != 0)
    {
        const std::string error = mysql_stmt_error(statement);
        mysql_stmt_close(statement);
        throw std::runtime_error("prepare SQL: " + error);
    }

    statements_[index] = statement;
}

void DbConnection::Close() noexcept
{
    for (MYSQL_STMT*& statement : statements_)
    {
        if (statement != nullptr)
        {
            mysql_stmt_close(statement);
            statement = nullptr;
        }
    }

    if (connection_ != nullptr)
    {
        mysql_close(connection_);
        connection_ = nullptr;
    }

    if (mysql_thread_initialized_)
    {
        mysql_thread_end();
        mysql_thread_initialized_ = false;
    }
}

std::string DbConnection::StatementError(MYSQL_STMT* statement) const
{
    const char* detail = mysql_stmt_error(statement);
    return detail == nullptr ? "unknown MySQL statement error" : detail;
}

DbResult DbConnection::Execute(const DbRequest& request)
{
    const auto started = std::chrono::steady_clock::now();
    const auto finish = [started](DbResult result)
    {
        result.database_call_us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started).count());
        return result;
    };

    std::size_t statement_index = kReadStatement;
    switch (request.operation)
    {
        case DbOperation::Read:
            statement_index = kReadStatement;
            break;
        case DbOperation::Insert:
            statement_index = kInsertStatement;
            break;
        case DbOperation::Update:
            statement_index = kUpdateStatement;
            break;
        case DbOperation::Delete:
            statement_index = kDeleteStatement;
            break;
    }

    MYSQL_STMT* statement = statements_[statement_index];
    // 这条语句在连接建立时已经 prepare；上一次查询结果也已释放，直接重新绑定参数即可。
    // 每次执行前再 reset 会额外让客户端和 MySQL 服务端重置一次语句状态。

    unsigned long long key = static_cast<unsigned long long>(request.key);
    unsigned long long value = static_cast<unsigned long long>(request.value);
    MYSQL_BIND parameters[2]{};
    BindUnsignedValue(parameters[0], &key);
    BindUnsignedValue(parameters[1], &value);

    if (mysql_stmt_bind_param(statement, parameters) != 0)
    {
        return finish({false, 0, StatementError(statement)});
    }

    if (mysql_stmt_execute(statement) != 0)
    {
        return finish({false, 0, StatementError(statement)});
    }

    if (request.operation == DbOperation::Read)
    {
        unsigned long long result_value = 0;
        MYSQL_BIND result_binding[1]{};
        BindUnsignedValue(result_binding[0], &result_value);

        if (mysql_stmt_bind_result(statement, result_binding) != 0)
        {
            mysql_stmt_free_result(statement);
            return finish({false, 0, StatementError(statement)});
        }
        const int fetch_result = mysql_stmt_fetch(statement);
        mysql_stmt_free_result(statement);
        if (fetch_result != 0)
        {
            return finish({false, 0, fetch_result == MYSQL_NO_DATA ? "read key was not found" : StatementError(statement)});
        }

        return finish({true, static_cast<std::uint64_t>(result_value), {}});
    }

    const my_ulonglong affected_rows = mysql_stmt_affected_rows(statement);
    if (affected_rows != 1U)
    {
        return finish({false, 0, "expected to change one row, changed " + std::to_string(affected_rows)});
    }

    return finish({true, static_cast<std::uint64_t>(affected_rows), {}});
}

DbPool::DbPool(DbConfig config, std::size_t worker_count)
    : config_(std::move(config)),
      worker_count_(worker_count)
{
    if (worker_count_ == 0)
    {
        throw std::invalid_argument("database worker count must be greater than zero");
    }

    workers_.reserve(worker_count_);
    for (std::size_t i = 0; i < worker_count_; ++i)
    {
        workers_.emplace_back([this]
            {
                WorkerMain();
            });
    }

    std::unique_lock<std::mutex> lock(startup_mutex_);
    startup_ready_.wait(lock, [this]
        {
            return startup_finished_ == worker_count_;
        });

    if (!startup_error_.empty())
    {
        lock.unlock();
        {
            std::lock_guard<std::mutex> queue_lock(queue_mutex_);
            stopping_ = true;
        }
        queue_ready_.notify_all();
        for (std::thread& worker : workers_)
        {
            if (worker.joinable())
            {
                worker.join();
            }
        }
        throw std::runtime_error("database pool startup failed: " + startup_error_);
    }
}

DbPool::~DbPool()
{
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        stopping_ = true;
    }
    queue_ready_.notify_all();

    for (std::thread& worker : workers_)
    {
        if (worker.joinable())
        {
            worker.join();
        }
    }
}

void DbPool::Submit(Job job)
{
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (stopping_)
        {
            throw std::runtime_error("database pool is stopping");
        }
        jobs_.push_back(std::move(job));
    }
    queue_ready_.notify_one();
}

std::size_t DbPool::WorkerCount() const noexcept
{
    return worker_count_;
}

void DbPool::WorkerMain()
{
    std::unique_ptr<DbConnection> connection;
    try
    {
        connection = std::make_unique<DbConnection>(config_);
    }
    catch (const std::exception& error)
    {
        {
            std::lock_guard<std::mutex> lock(startup_mutex_);
            if (startup_error_.empty())
            {
                startup_error_ = error.what();
            }
            ++startup_finished_;
        }
        startup_ready_.notify_one();
        return;
    }

    {
        std::lock_guard<std::mutex> lock(startup_mutex_);
        ++startup_finished_;
    }
    startup_ready_.notify_one();

    for (;;)
    {
        Job job;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_ready_.wait(lock, [this]
                {
                    return stopping_ || !jobs_.empty();
                });

            if (stopping_ && jobs_.empty())
            {
                return;
            }

            job = std::move(jobs_.front());
            jobs_.pop_front();
        }

        try
        {
            job(*connection);
        }
        catch (...)
        {
            // 每个任务的异常应由提交任务的一方处理；不能让一个异常杀死整个数据库工作线程。
        }
    }
}

DbOperationAwaiter::DbOperationAwaiter(DbPool& pool, nebula::net::EventLoop* loop, DbRequest request)
    : pool_(pool),
      loop_(loop),
      request_(request)
{
    if (loop_ == nullptr)
    {
        throw std::invalid_argument("database operation requires an EventLoop");
    }
}

bool DbOperationAwaiter::await_ready() const noexcept
{
    return false;
}

void DbOperationAwaiter::await_suspend(std::coroutine_handle<> continuation)
{
    const auto submitted_at = std::chrono::steady_clock::now();
    pool_.Submit([this, continuation, submitted_at](DbConnection& connection)
        {
            const auto database_started_at = std::chrono::steady_clock::now();
            try
            {
                result_ = connection.Execute(request_);
            }
            catch (const std::exception& error)
            {
                result_ = {false, 0, error.what()};
            }
            catch (...)
            {
                result_ = {false, 0, "unknown database error"};
            }

            // 从提交任务到数据库线程真正开始执行，中间经过的时间就是线程池排队时间。
            result_.queue_wait_us = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    database_started_at - submitted_at).count());

            // 数据库操作完成后回到原来的 EventLoop，不能在数据库线程里恢复 RPC 协程。
            loop_->QueueInLoop([continuation]
                {
                    continuation.resume();
                });
        });
}

DbResult DbOperationAwaiter::await_resume() noexcept
{
    return std::move(result_);
}

}  // namespace ordersystem::benchmark
