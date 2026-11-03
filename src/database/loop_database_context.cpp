#include "ordersystem/database/loop_database_context.h"

#include <charconv>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace ordersystem::benchmark
{
namespace
{

constexpr std::size_t kReadStatement = 0;
constexpr std::size_t kInsertStatement = 1;
constexpr std::size_t kUpdateStatement = 2;
constexpr std::size_t kDeleteStatement = 3;

constexpr std::uint64_t kPostgresBigintMax =
    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

using PgResult = std::unique_ptr<PGresult, decltype(&PQclear)>;

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

std::string BigintParameter(std::uint64_t value, const char* name)
{
    if (value > kPostgresBigintMax)
    {
        throw std::invalid_argument(std::string(name) + " exceeds PostgreSQL BIGINT range");
    }
    return std::to_string(value);
}

}  // namespace

LoopDatabaseContext::LoopDatabaseContext(nebula::net::EventLoop* loop, DbConfig config)
    : loop_(loop),
      config_(std::move(config))
{
    if (loop_ == nullptr)
    {
        throw std::invalid_argument("LoopDatabaseContext requires an EventLoop");
    }

    loop_->AssertInLoopThread();
    ConnectAndPrepare();
}

LoopDatabaseContext::~LoopDatabaseContext()
{
    Close();
}

nebula::net::EventLoop* LoopDatabaseContext::Loop() const noexcept
{
    return loop_;
}

void LoopDatabaseContext::AssertInOwnerLoop() const
{
    loop_->AssertInLoopThread();
}

// 阶段 2 先把“请求执行路径”改成完全非阻塞。
// 建连、设置编码和准备四条固定 SQL 仍在 IO Loop 启动前同步完成，只发生一次，不参与每次 RPC 的热路径。
void LoopDatabaseContext::ConnectAndPrepare()
{
    const std::string port = std::to_string(config_.port);
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
        config_.host.c_str(),
        port.c_str(),
        config_.user.c_str(),
        config_.password.empty() ? nullptr : config_.password.c_str(),
        config_.database.c_str(),
        "5",
        "ordersystem_db_benchmark_async",
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

        // 从这里开始，请求执行只能走 PQsend*/PQconsumeInput/PQgetResult 这一套异步接口。
        // PQexec/PQexecPrepared 即使连接被设成 nonblocking 也仍然会阻塞，因此阶段 2 不再调用它们。
        if (PQsetnonblocking(connection_, 1) != 0)
        {
            throw std::runtime_error("enable PostgreSQL nonblocking mode: " + ConnectionError());
        }

        const int socket_fd = PQsocket(connection_);
        if (socket_fd < 0)
        {
            throw std::runtime_error("PostgreSQL connection has no valid socket");
        }

        channel_ = std::make_unique<nebula::net::Channel>(loop_, socket_fd);
        channel_->SetReadCallback([this]
            {
                OnReadable();
            });
        channel_->SetWriteCallback([this]
            {
                OnWritable();
            });
        channel_->SetCloseCallback([this]
            {
                OnSocketClosed();
            });
        channel_->SetErrorCallback([this]
            {
                OnSocketError();
            });

        // 平时始终关心数据库返回的数据；只有 PQflush() 表示还有数据没写完时才额外监听可写事件。
        channel_->EnableReading();
    }
    catch (...)
    {
        Close();
        throw;
    }
}

void LoopDatabaseContext::Prepare(std::size_t index,
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

void LoopDatabaseContext::Submit(DbRequest request,
                                 std::coroutine_handle<> continuation,
                                 DbResult* result)
{
    loop_->AssertInLoopThread();

    if (result == nullptr)
    {
        throw std::invalid_argument("database result storage must not be null");
    }

    PendingRequest pending{
        request,
        continuation,
        result,
        Clock::now()
    };

    if (connection_failed_)
    {
        *result = {false, 0, connection_failure_};
        ResumeLater(continuation);
        return;
    }

    pending_requests_.push_back(std::move(pending));
    StartNextRequest();
}

// 阶段 2 不使用 Pipeline，因此同一条连接上只有 active_request_ 这一条 SQL 在途。
// 其他 RPC 请求只在当前 EventLoop 自己的队列里等待，不需要 mutex 和 condition_variable。
void LoopDatabaseContext::StartNextRequest()
{
    loop_->AssertInLoopThread();

    if (connection_failed_ || active_request_.has_value() || pending_requests_.empty())
    {
        return;
    }

    active_request_.emplace(std::move(pending_requests_.front()));
    pending_requests_.pop_front();
    active_result_.reset();
    active_started_at_ = Clock::now();

    active_request_->result->queue_wait_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            active_started_at_ - active_request_->submitted_at).count());

    if (!SendActiveRequest())
    {
        return;
    }

    FlushOutput();
}

bool LoopDatabaseContext::SendActiveRequest()
{
    if (!active_request_.has_value())
    {
        return false;
    }

    if (PQstatus(connection_) != CONNECTION_OK)
    {
        FailConnection("PostgreSQL connection is not available: " + ConnectionError());
        return false;
    }

    const DbRequest& request = active_request_->request;

    if (request.key == 0 || request.key > kPostgresBigintMax)
    {
        CompleteActive({false, 0, "database key must be between 1 and PostgreSQL BIGINT max"});
        return false;
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
            CompleteActive({false, 0, "unsupported database operation"});
            return false;
    }

    try
    {
        const std::string key = BigintParameter(request.key, "database key");
        const std::string value = request.operation == DbOperation::Insert
            ? BigintParameter(request.value, "database value")
            : std::string{};
        const char* parameters[] = {key.c_str(), value.c_str()};

        // 这里只把 SQL 请求交给 libpq 的输出缓冲区，不等待 PostgreSQL 返回。
        // 真正的结果稍后由 socket 可读事件进入 OnReadable()。
        if (PQsendQueryPrepared(connection_,
                                statement_names_[statement_index],
                                parameter_count,
                                parameters,
                                nullptr,
                                nullptr,
                                0) != 1)
        {
            // 当前只有在上一条命令已经完整结束后才会走到这里。
            // 因此发送失败不再把它当成某一个请求的普通业务错误，而是直接认为这条连接已经不可继续使用。
            FailConnection("send PostgreSQL query: " + ConnectionError());
            return false;
        }
    }
    catch (const std::exception& error)
    {
        CompleteActive({false, 0, error.what()});
        return false;
    }

    return true;
}

// 非阻塞连接下，PQsendQueryPrepared() 可能只把数据放进 libpq 自己的缓冲区。
// PQflush()==1 表示还没完全写进 socket，此时让 EventLoop 等 EPOLLOUT；绝不能在这里循环等它写完。
bool LoopDatabaseContext::FlushOutput()
{
    if (connection_failed_)
    {
        return false;
    }

    const int flush_result = PQflush(connection_);
    if (flush_result < 0)
    {
        FailConnection("flush PostgreSQL output: " + ConnectionError());
        return false;
    }

    if (flush_result == 1)
    {
        if (!channel_->IsWriting())
        {
            channel_->EnableWriting();
        }
        return true;
    }

    if (channel_->IsWriting())
    {
        channel_->DisableWriting();
    }
    return true;
}

void LoopDatabaseContext::OnReadable()
{
    loop_->AssertInLoopThread();

    if (connection_failed_)
    {
        return;
    }

    // socket 已经被 epoll 判断为可读，所以 PQconsumeInput() 这里只读取当前已经到达的数据，不等待网络。
    if (PQconsumeInput(connection_) != 1)
    {
        FailConnection("consume PostgreSQL input: " + ConnectionError());
        return;
    }

    // PostgreSQL 可能在我们还有输出没写完时先发回 NOTICE 等数据。
    // 先把可读数据清掉，再继续尝试 flush，避免两边互相等。
    if (!FlushOutput())
    {
        return;
    }

    ProcessResults();
}

void LoopDatabaseContext::OnWritable()
{
    loop_->AssertInLoopThread();

    if (connection_failed_)
    {
        return;
    }

    FlushOutput();
}

void LoopDatabaseContext::OnSocketClosed()
{
    loop_->AssertInLoopThread();
    FailConnection("PostgreSQL socket was closed");
}

void LoopDatabaseContext::OnSocketError()
{
    loop_->AssertInLoopThread();
    FailConnection("PostgreSQL socket error: " + ConnectionError());
}

// PQconsumeInput() 已经把当前可读数据搬进 libpq 缓冲区。
// 只有 PQisBusy()==0 时，PQgetResult() 才保证不会等待新的网络数据。
void LoopDatabaseContext::ProcessResults()
{
    if (!active_request_.has_value())
    {
        return;
    }

    while (PQisBusy(connection_) == 0)
    {
        PgResult result(PQgetResult(connection_), &PQclear);

        if (result == nullptr)
        {
            // PQgetResult() 返回 nullptr 才表示这条命令已经完整结束，可以安全发送下一条 SQL。
            DbResult completed = active_result_.has_value()
                ? std::move(*active_result_)
                : DbResult{false, 0, "PostgreSQL command completed without a result"};
            CompleteActive(std::move(completed));
            return;
        }

        if (active_result_.has_value())
        {
            // 当前每次只发送一条预处理 SQL，理论上只应得到一个非空 PGresult。
            active_result_ = DbResult{false, 0, "PostgreSQL returned multiple results for one prepared statement"};
            continue;
        }

        active_result_ = ParseResult(result.get(), active_request_->request.operation);

        // 即使当前 PGresult 表示失败，也继续把 PQgetResult() 读到 nullptr。
        // libpq 要求把当前命令的结果完整取完后，连接才能安全接收下一条命令。
    }
}

DbResult LoopDatabaseContext::ParseResult(PGresult* result, DbOperation operation) const
{
    if (result == nullptr)
    {
        return {false, 0, "PostgreSQL returned an empty result"};
    }

    if (operation == DbOperation::Read)
    {
        if (PQresultStatus(result) != PGRES_TUPLES_OK)
        {
            return {false, 0, ResultError(connection_, result)};
        }

        if (PQntuples(result) == 0)
        {
            return {false, 0, "read key was not found"};
        }

        if (PQgetisnull(result, 0, 0) != 0)
        {
            return {false, 0, "read value is NULL"};
        }

        std::uint64_t result_value = 0;
        const char* raw_value = PQgetvalue(result, 0, 0);
        if (raw_value == nullptr || !ParseUnsigned(raw_value, &result_value))
        {
            return {false, 0, "PostgreSQL returned an invalid BIGINT value"};
        }

        return {true, result_value, {}};
    }

    if (PQresultStatus(result) != PGRES_COMMAND_OK)
    {
        return {false, 0, ResultError(connection_, result)};
    }

    std::uint64_t affected_rows = 0;
    const char* affected_text = PQcmdTuples(result);
    if (affected_text == nullptr || !ParseUnsigned(affected_text, &affected_rows))
    {
        return {false, 0, "PostgreSQL did not return a valid affected-row count"};
    }

    if (affected_rows != 1U)
    {
        return {false, 0, "expected to change one row, changed " +
                              std::to_string(affected_rows)};
    }

    return {true, affected_rows, {}};
}

void LoopDatabaseContext::CompleteActive(DbResult result)
{
    if (!active_request_.has_value())
    {
        return;
    }

    result.queue_wait_us = active_request_->result->queue_wait_us;
    result.database_call_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now() - active_started_at_).count());

    DbResult* result_storage = active_request_->result;
    const std::coroutine_handle<> continuation = active_request_->continuation;

    *result_storage = std::move(result);
    active_request_.reset();
    active_result_.reset();

    // 当前就在原 EventLoop 线程里，不再需要跨线程唤醒。
    // 仍然 QueueInLoop 而不是直接 resume，避免在 Channel 回调栈里继续执行整段 RPC 业务代码。
    ResumeLater(continuation);

    if (!connection_failed_)
    {
        StartNextRequest();
    }
}

void LoopDatabaseContext::FailConnection(std::string error)
{
    if (connection_failed_)
    {
        return;
    }

    connection_failed_ = true;
    connection_failure_ = std::move(error);

    if (channel_ != nullptr && loop_->HasChannel(channel_.get()))
    {
        channel_->DisableAll();
    }

    if (active_request_.has_value())
    {
        DbResult failed{false, 0, connection_failure_};
        failed.queue_wait_us = active_request_->result->queue_wait_us;
        failed.database_call_us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                Clock::now() - active_started_at_).count());

        *active_request_->result = std::move(failed);
        const std::coroutine_handle<> continuation = active_request_->continuation;
        active_request_.reset();
        active_result_.reset();
        ResumeLater(continuation);
    }

    const TimePoint failed_at = Clock::now();
    while (!pending_requests_.empty())
    {
        PendingRequest pending = std::move(pending_requests_.front());
        pending_requests_.pop_front();

        pending.result->success = false;
        pending.result->error = connection_failure_;
        pending.result->queue_wait_us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                failed_at - pending.submitted_at).count());
        ResumeLater(pending.continuation);
    }
}

void LoopDatabaseContext::ResumeLater(std::coroutine_handle<> continuation)
{
    loop_->QueueInLoop([continuation]
        {
            if (continuation)
            {
                continuation.resume();
            }
        });
}

void LoopDatabaseContext::Close() noexcept
{
    if (channel_ != nullptr)
    {
        try
        {
            if (loop_ != nullptr && loop_->HasChannel(channel_.get()))
            {
                channel_->DisableAll();
                channel_->Remove();
            }
        }
        catch (...)
        {
        }
        channel_.reset();
    }

    if (connection_ != nullptr)
    {
        PQfinish(connection_);
        connection_ = nullptr;
    }
}

std::string LoopDatabaseContext::ConnectionError() const
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

DbOperationAwaiter::DbOperationAwaiter(LoopDatabaseContext& database, DbRequest request)
    : database_(database),
      request_(request)
{
}

bool DbOperationAwaiter::await_ready() const noexcept
{
    return false;
}

void DbOperationAwaiter::await_suspend(std::coroutine_handle<> continuation)
{
    database_.Submit(request_, continuation, &result_);
}

DbResult DbOperationAwaiter::await_resume() noexcept
{
    return std::move(result_);
}

}  // namespace ordersystem::benchmark
