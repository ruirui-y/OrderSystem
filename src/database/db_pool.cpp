#include "ordersystem/database/db_pool.h"

#include <charconv>
#include <chrono>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace ordersystem::benchmark
{
namespace
{
// 下标与 DbOperation 的四种操作一一对应，用来找到连接上提前准备好的 SQL。
constexpr std::size_t kReadStatement = 0;
constexpr std::size_t kInsertStatement = 1;
constexpr std::size_t kUpdateStatement = 2;
constexpr std::size_t kDeleteStatement = 3;

// PostgreSQL BIGINT 是有符号整数，传入 uint64_t 前必须检查是否超出它的最大值。
constexpr std::uint64_t kPostgresBigintMax =
    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

// PGresult 必须用 PQclear() 释放；交给 unique_ptr 管理，提前返回时也不会漏掉清理。
using PgResult = std::unique_ptr<PGresult, decltype(&PQclear)>;

// 优先返回当前 SQL 的错误；结果中没有错误信息时，再读取连接上的错误。
std::string ResultError(PGconn* connection, PGresult* result)
{
    const char* message = result == nullptr ? nullptr : PQresultErrorMessage(result);
    if (message == nullptr || message[0] == '\0')
    {
        message = connection == nullptr ? nullptr : PQerrorMessage(connection);
    }

    if (message == nullptr || message[0] == '\0')
    {
        return "unknown PostgreSQL error";
    }

    std::string error(message);
    while (!error.empty() && (error.back() == '\n' || error.back() == '\r'))
    {
        error.pop_back();
    }
    return error;
}

// 必须完整解析数字文本，像 "123abc" 这样的内容不能只取前面的数字。
bool ParseUnsigned(std::string_view text, std::uint64_t* value)
{
    if (text.empty() || value == nullptr)
    {
        return false;
    }

    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto [parsed_end, error] = std::from_chars(begin, end, *value);
    return error == std::errc{} && parsed_end == end;
}

// PostgreSQL BIGINT 是有符号整数，超出范围时在发送 SQL 前直接报错。
std::string BigintParameter(std::uint64_t value, const char* name)
{
    if (value > kPostgresBigintMax)
    {
        throw std::invalid_argument(std::string(name) + " exceeds PostgreSQL BIGINT range");
    }
    return std::to_string(value);
}

}  // namespace

// 每个 DbConnection 独占一条 PostgreSQL 连接；当前由 DB Worker 使用同步接口操作它。
DbConnection::DbConnection(const DbConfig& config)
{
    // libpq 通过两组一一对应的字符串数组读取连接配置，数组末尾都用 nullptr 标记结束。
    const std::string port = std::to_string(config.port);  // libpq 的连接参数值都使用字符串，端口也要先转成文本。
    const char* keywords[] = {
        "host",
        "port",
        "user",
        "password",
        "dbname",
        "connect_timeout",
        "application_name",
        nullptr
    };
    const char* values[] = {
        config.host.c_str(),
        port.c_str(),
        config.user.c_str(),
        config.password.empty() ? nullptr : config.password.c_str(),
        config.database.c_str(),
        "5",
        "ordersystem_db_benchmark",
        nullptr
    };

    connection_ = PQconnectdbParams(keywords, values, 0);
    if (connection_ == nullptr)
    {
        throw std::runtime_error("allocate PostgreSQL connection failed");
    }

    if (PQstatus(connection_) != CONNECTION_OK)
    {
        const std::string error = ConnectionError();
        Close();
        throw std::runtime_error("connect to PostgreSQL: " + error);
    }

    if (PQsetClientEncoding(connection_, "UTF8") != 0)
    {
        const std::string error = ConnectionError();
        Close();
        throw std::runtime_error("set PostgreSQL client encoding: " + error);
    }

    try
    {
        // 每条连接只需准备一次 SQL，后续请求直接传参数执行。
        // 默认自动提交，每条 SQL 单独提交；显式执行 BEGIN 后才进入事务。
        Prepare(kReadStatement,
                "ordersystem_read",
                "SELECT value FROM bench_read_records WHERE id = $1",
                1);
        Prepare(kInsertStatement,
                "ordersystem_insert",
                "INSERT INTO bench_insert_records (id, value) VALUES ($1, $2)",
                2);
        Prepare(kUpdateStatement,
                "ordersystem_update",
                "UPDATE bench_update_records SET value = value + 1 WHERE id = $1",
                1);
        Prepare(kDeleteStatement,
                "ordersystem_delete",
                "DELETE FROM bench_delete_records WHERE id = $1",
                1);
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

// 预处理语句只在创建它的连接上有效，因此每个 Worker 都要为自己的连接准备一份。
void DbConnection::Prepare(std::size_t index,
                           const char* name,
                           const char* sql,
                           int parameter_count)
{
    PgResult result(PQprepare(connection_, name, sql, parameter_count, nullptr), &PQclear);
    if (result == nullptr || PQresultStatus(result.get()) != PGRES_COMMAND_OK)
    {
        throw std::runtime_error("prepare PostgreSQL statement " + std::string(name) +
                                 ": " + ResultError(connection_, result.get()));
    }

    statement_names_[index] = name;
}

void DbConnection::Close() noexcept
{
    if (connection_ != nullptr)
    {
        PQfinish(connection_);
        connection_ = nullptr;
    }
}

std::string DbConnection::ConnectionError() const
{
    const char* message = connection_ == nullptr ? nullptr : PQerrorMessage(connection_);
    if (message == nullptr || message[0] == '\0')
    {
        return "unknown PostgreSQL connection error";
    }

    std::string error(message);
    while (!error.empty() && (error.back() == '\n' || error.back() == '\r'))
    {
        error.pop_back();
    }
    return error;
}

// 同步执行已经准备好的 SQL，调用线程会一直等到 PostgreSQL 返回结果；只能由 DB Worker 调用。
DbResult DbConnection::Execute(const DbRequest& request)
{
    // 统一记录从开始执行到拿到数据库结果的耗时，所有返回路径都会经过 finish。
    const auto started = std::chrono::steady_clock::now();
    const auto finish = [started](DbResult result)
    {
        result.database_call_us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started).count());
        return result;
    };

    // 压测数据的主键从 1 开始，并且必须能用 PostgreSQL BIGINT 表示。
    if (request.key == 0 || request.key > kPostgresBigintMax)
    {
        return finish({false, 0, "database key must be between 1 and PostgreSQL BIGINT max"});
    }

    std::size_t statement_index = kReadStatement;
    int parameter_count = 1;
    switch (request.operation)
    {
        case DbOperation::Read:
            statement_index = kReadStatement;
            break;
        case DbOperation::Insert:
            statement_index = kInsertStatement;
            parameter_count = 2;
            break;
        case DbOperation::Update:
            statement_index = kUpdateStatement;
            break;
        case DbOperation::Delete:
            statement_index = kDeleteStatement;
            break;
        default:
            return finish({false, 0, "unsupported database operation"});
    }

    try
    {
        // 参数通过 $1、$2 传给 SQL，不拼接到 SQL 字符串中，避免把用户数据当成 SQL 代码执行。
        const std::string key = BigintParameter(request.key, "database key");
        const std::string value = request.operation == DbOperation::Insert
            ? BigintParameter(request.value, "database value")
            : std::string{};
        const char* parameters[] = {key.c_str(), value.c_str()};

        PgResult result(PQexecPrepared(connection_,
                                       statement_names_[statement_index],
                                       parameter_count,
                                       parameters,
                                       nullptr,
                                       nullptr,
                                       0),
                       &PQclear);
        if (result == nullptr)
        {
            return finish({false, 0, ConnectionError()});
        }

        if (request.operation == DbOperation::Read)
        {
            // SELECT 返回的是结果行集合，成功状态为 PGRES_TUPLES_OK。
            if (PQresultStatus(result.get()) != PGRES_TUPLES_OK)
            {
                return finish({false, 0, ResultError(connection_, result.get())});
            }

            if (PQntuples(result.get()) == 0)
            {
                return finish({false, 0, "read key was not found"});
            }

            if (PQgetisnull(result.get(), 0, 0) != 0)
            {
                return finish({false, 0, "read value is NULL"});
            }

            // 这条查询只返回一行一列，取第 0 行第 0 列，并转换成 uint64_t。
            std::uint64_t result_value = 0;
            const char* raw_value = PQgetvalue(result.get(), 0, 0);
            if (raw_value == nullptr || !ParseUnsigned(raw_value, &result_value))
            {
                return finish({false, 0, "PostgreSQL returned an invalid BIGINT value"});
            }

            return finish({true, result_value, {}});
        }

        if (PQresultStatus(result.get()) != PGRES_COMMAND_OK)
        {
            return finish({false, 0, ResultError(connection_, result.get())});
        }

        // 压测要求每个 key 都对应一行，因此写操作影响的行数必须恰好为 1。
        std::uint64_t affected_rows = 0;
        const char* affected_text = PQcmdTuples(result.get());
        if (affected_text == nullptr || !ParseUnsigned(affected_text, &affected_rows))
        {
            return finish({false, 0, "PostgreSQL did not return a valid affected-row count"});
        }

        if (affected_rows != 1U)
        {
            return finish({false, 0, "expected to change one row, changed " +
                                          std::to_string(affected_rows)});
        }

        return finish({true, affected_rows, {}});
    }
    catch (const std::exception& error)
    {
        return finish({false, 0, error.what()});
    }
}

// 创建固定数量的 Worker，并等所有 PostgreSQL 连接初始化完成后再继续启动服务。
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
        // 任意一条连接初始化失败，都要停止其他 Worker，避免服务带着不完整的连接池启动。
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

// 停止接收新任务；Worker 会处理完队列中已有的任务再退出，join 等待所有线程结束。
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

void DbPool::Submit(Job job)  // 入队时短暂持锁，执行 SQL 时不持有队列锁。
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

// 每个 Worker 在线程内部建立并独占一条 PostgreSQL 连接，然后不断取出任务执行。
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
            // 队列为空时睡眠，直到有新任务或线程池开始关闭。
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
            // 单个任务抛异常不能让 Worker 退出，否则线程池可用的数据库连接会减少。
        }
    }
}

// loop_ 必须有效，数据库操作完成后需要把协程恢复到它所属的 EventLoop。
DbOperationAwaiter::DbOperationAwaiter(DbPool& pool,
                                       nebula::net::EventLoop* loop,
                                       DbRequest request)
    : pool_(pool),
      loop_(loop),
      request_(request)
{
    if (loop_ == nullptr)
    {
        throw std::invalid_argument("database operation requires an EventLoop");
    }
}

// 数据库任务交给 Worker 执行，因此这里始终返回 false。
bool DbOperationAwaiter::await_ready() const noexcept
{
    return false;
}

// 提交数据库任务并挂起协程；Worker 完成后，将恢复动作投回原来的 EventLoop。
void DbOperationAwaiter::await_suspend(std::coroutine_handle<> continuation)
{
    // 统计任务从提交到 Worker 取出的等待时间，不包含 SQL 执行时间。
    const auto submitted_at = std::chrono::steady_clock::now();
    pool_.Submit([this, continuation, submitted_at](DbConnection& connection)
        {
            // Worker 取到任务后开始计时，之前的时间只计入队列等待。
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

            result_.queue_wait_us = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    database_started_at - submitted_at).count());

            // 不能直接在 DB Worker 线程恢复协程，否则后续业务代码会跑错线程。
            // 投回原来的 EventLoop 后，再由它执行 continuation.resume()。
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
