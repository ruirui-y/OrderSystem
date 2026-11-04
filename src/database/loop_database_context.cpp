#include "ordersystem/database/loop_database_context.h"

#include "nebula/base/logger.h"

#include <charconv>
#include <cstdint>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

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

// libpq 的错误信息末尾通常带换行，拼进异常消息或日志前先去掉。
std::string TrimTrailingNewlines(std::string text)
{
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
    {
        text.pop_back();
    }
    return text;
}

// 优先使用结果自带的错误信息；结果本身没有说明时，退回连接级错误。
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

    return TrimTrailingNewlines(message);
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

// 把无符号整数转成 PostgreSQL 参数文本。key/value 在 ValidateRequest 里已经检查过上限，
// 这里保留同样的检查作为最后一道防线：与其把超出 BIGINT 的值发出去，不如在这里失败。
std::string BigintParameter(std::uint64_t value, const char* name)
{
    if (value > kPostgresBigintMax)
    {
        throw std::invalid_argument(std::string(name) + " exceeds PostgreSQL BIGINT range");
    }
    return std::to_string(value);
}

// 日志里用可读名字标记数据库操作，避免只看到枚举下标。
const char* OperationName(DbOperation operation)
{
    switch (operation)
    {
        case DbOperation::Read:
            return "read";
        case DbOperation::Insert:
            return "insert";
        case DbOperation::Update:
            return "update";
        case DbOperation::Delete:
            return "delete";
    }
    return "unknown";
}

// spdlog 不能直接格式化 std::thread::id，先转成字符串。
std::string ThreadId()
{
    std::ostringstream stream;
    stream << std::this_thread::get_id();
    return stream.str();
}

// 协程句柄本身不能直接打印，统一转成地址，方便在日志里对照同一个 continuation。
std::uintptr_t ContinuationAddress(std::coroutine_handle<> continuation)
{
    return reinterpret_cast<std::uintptr_t>(continuation.address());
}

}  // namespace

LoopDatabaseContext::LoopDatabaseContext(nebula::net::EventLoop* loop,
                                         DbConfig config,
                                         std::size_t max_outstanding_requests)
    : loop_(loop),
      config_(std::move(config)),
      max_outstanding_requests_(max_outstanding_requests)
{
    if (loop_ == nullptr)
    {
        throw std::invalid_argument("LoopDatabaseContext requires an EventLoop");
    }
    if (max_outstanding_requests_ == 0)
    {
        throw std::invalid_argument("database outstanding request limit must be greater than zero");
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
        "ordersystem_db_benchmark_pipeline",
        nullptr
    };

    // 建连、设置编码、准备语句、切换非阻塞和 Pipeline 属于同一次初始化，
    // 任何一步失败都交给下面的 catch 统一释放已经建立的连接。
    try
    {
        connection_ = PQconnectdbParams(keywords, values, 0);
        if (connection_ == nullptr)
        {
            throw std::runtime_error("allocate PostgreSQL connection failed");
        }

        if (PQstatus(connection_) != CONNECTION_OK)
        {
            throw std::runtime_error("connect to PostgreSQL: " + ConnectionError());
        }

        if (PQsetClientEncoding(connection_, "UTF8") != 0)
        {
            throw std::runtime_error("set PostgreSQL client encoding: " + ConnectionError());
        }

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

        if (PQsetnonblocking(connection_, 1) != 0)
        {
            throw std::runtime_error("enable PostgreSQL nonblocking mode: " + ConnectionError());
        }

        // 预处理语句已经在普通模式下准备完成。进入 Pipeline 后只使用异步发送接口。
        if (PQenterPipelineMode(connection_) != 1)
        {
            throw std::runtime_error("enter PostgreSQL pipeline mode: " + ConnectionError());
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

    if (connection_failed_)
    {
        *result = {false, 0, connection_failure_};
        ResumeLater(continuation);
        return;
    }

    // 在途请求和已出结果但还没等到同步点的请求都占名额，避免连接上的请求无限增长。
    if (in_flight_requests_.size() + completed_requests_.size() >= max_outstanding_requests_)
    {
        *result = {false, 0, "database request queue is full"};
        ResumeLater(continuation);
        return;
    }

    const std::string validation_error = ValidateRequest(request);
    if (!validation_error.empty())
    {
        *result = {false, 0, validation_error};
        ResumeLater(continuation);
        return;
    }

    const TimePoint now = Clock::now();
    InFlightRequest in_flight;
    in_flight.request = std::move(request);
    in_flight.continuation = continuation;
    in_flight.result = result;
    in_flight.submitted_at = now;
    in_flight.sent_at = now;
    in_flight_requests_.push_back(std::move(in_flight));

    NLOG_DEBUG("[db] Submit op={} key={} cont=0x{:x} tid={} inflight={} completed={} "
               "sync_pending={} sync_scheduled={}",
               OperationName(in_flight_requests_.back().request.operation),
               in_flight_requests_.back().request.key,
               ContinuationAddress(continuation),
               ThreadId(),
               in_flight_requests_.size(),
               completed_requests_.size(),
               sync_pending_,
               sync_scheduled_);

    // 每个请求到达就立刻发送，不等待凑够固定数量。这里只负责发出去，
    // 结果由 OnReadable 异步取回，Submit 本身不阻塞。
    if (!SendInFlightRequest(in_flight_requests_.back()))
    {
        return;
    }

    SchedulePipelineSync();
}

// 同一轮事件处理里可能连续提交多个请求。QueueInLoop 的语义是"当前这一轮处理结束后再执行"，
// 所以这里只排一次队，让本轮发出的所有请求共用同一个同步点，而不是每条 SQL 都发一次 Sync。
void LoopDatabaseContext::SchedulePipelineSync()
{
    if (connection_failed_ || sync_scheduled_)
    {
        return;
    }

    sync_scheduled_ = true;
    sync_pending_ = true;

    NLOG_DEBUG("[db] SchedulePipelineSync tid={} inflight={} completed={} sync_pending={} "
               "sync_scheduled={}",
               ThreadId(),
               in_flight_requests_.size(),
               completed_requests_.size(),
               sync_pending_,
               sync_scheduled_);

    loop_->QueueInLoop([this]
        {
            sync_scheduled_ = false;
            SendPipelineSync();
        });
}

void LoopDatabaseContext::SendPipelineSync()
{
    if (connection_failed_ || !sync_pending_)
    {
        return;
    }

    sync_pending_ = false;

    const int sync_result = PQpipelineSync(connection_);
    NLOG_DEBUG("[db] SendPipelineSync PQpipelineSync={} inflight={} completed={} tid={}",
               sync_result,
               in_flight_requests_.size(),
               completed_requests_.size(),
               ThreadId());

    if (sync_result != 1)
    {
        FailConnection("send PostgreSQL pipeline sync: " + ConnectionError());
        return;
    }

    FlushOutput();
}

bool LoopDatabaseContext::SendInFlightRequest(InFlightRequest& request)
{
    const DbRequest& db_request = request.request;

    std::size_t statement_index = kReadStatement;
    int parameter_count = 1;
    switch (db_request.operation)
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
            FailConnection("unsupported database operation reached the pipeline sender");
            return false;
    }

    try
    {
        const std::string key = BigintParameter(db_request.key, "database key");
        const std::string value = db_request.operation == DbOperation::Insert
            ? BigintParameter(db_request.value, "database value")
            : std::string{};
        const char* parameters[] = {key.c_str(), value.c_str()};

        const int send_result = PQsendQueryPrepared(connection_,
                                                   statement_names_[statement_index],
                                                   parameter_count,
                                                   parameters,
                                                   nullptr,
                                                   nullptr,
                                                   0);

        NLOG_DEBUG("[db] SendInFlightRequest op={} key={} PQsendQueryPrepared={} inflight={} "
                   "completed={} sync_pending={}",
                   OperationName(db_request.operation),
                   db_request.key,
                   send_result,
                   in_flight_requests_.size(),
                   completed_requests_.size(),
                   sync_pending_);

        if (send_result != 1)
        {
            FailConnection("send PostgreSQL pipeline query: " + ConnectionError());
            return false;
        }
    }
    catch (const std::exception& error)
    {
        FailConnection("prepare PostgreSQL pipeline parameters: " + std::string(error.what()));
        return false;
    }

    return true;
}

bool LoopDatabaseContext::FlushOutput()
{
    if (connection_failed_)
    {
        return false;
    }

    const int flush_result = PQflush(connection_);

    NLOG_DEBUG("[db] FlushOutput PQflush={} inflight={} completed={} writing={} sync_pending={}",
               flush_result,
               in_flight_requests_.size(),
               completed_requests_.size(),
               channel_->IsWriting(),
               sync_pending_);

    if (flush_result < 0)
    {
        FailConnection("flush PostgreSQL output: " + ConnectionError());
        return false;
    }

    // PQflush 返回 1 表示还有数据没发完，需要等 socket 可写再来一次；返回 0 表示已经发完。
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
    // 可读事件的处理顺序固定：先把 socket 数据读进 libpq 缓冲区，
    // 再把还没发出去的数据刷出去，最后解析已经完整的结果。三步不能调换。
    loop_->AssertInLoopThread();

    if (connection_failed_)
    {
        return;
    }

    const int consume_result = PQconsumeInput(connection_);

    NLOG_DEBUG("[db] OnReadable PQconsumeInput={} PQisBusy={} inflight={} completed={} tid={}",
               consume_result,
               consume_result == 1 ? PQisBusy(connection_) : -1,
               in_flight_requests_.size(),
               completed_requests_.size(),
               ThreadId());

    if (consume_result != 1)
    {
        FailConnection("consume PostgreSQL input: " + ConnectionError());
        return;
    }

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

void LoopDatabaseContext::ProcessResults()
{
    // 一次可读事件里，PostgreSQL 可能已经把同一批多条 SQL 的结果一起发过来了，
    // 所以这里要一直取到没有结果为止，而不是取到一条就返回。
    // 还要注意：Pipeline 模式下取走最后一条结果后，PQgetResult() 会返回 nullptr，
    // 而 PQisBusy() 仍然是 0。只看其中任何一个都会卡住，所以用"连续两次取不到结果"
    // 来判定本批读完；中间只要拿到一条 PGresult 就重新计数。
    std::size_t consecutive_empty_results = 0;

    while (PQisBusy(connection_) == 0)
    {
        PgResult result(PQgetResult(connection_), &PQclear);
        if (result == nullptr)
        {
            // 第一个 nullptr 表示当前 SQL 的结果已经结束，继续尝试取后面 SQL 的结果。
            ++consecutive_empty_results;
            if (consecutive_empty_results >= 2)
            {
                // 连续两次都没有新结果，说明这一批已经读完，回到 EventLoop 让排队任务得以执行。
                break;
            }
            continue;
        }

        consecutive_empty_results = 0;

        const ExecStatusType status = PQresultStatus(result.get());

        NLOG_DEBUG("[db] ProcessResults status={} is_pipeline_sync={} PQisBusy={} inflight={} "
                   "completed={}",
                   PQresStatus(status),
                   status == PGRES_PIPELINE_SYNC,
                   PQisBusy(connection_),
                   in_flight_requests_.size(),
                   completed_requests_.size());
        // PGRES_PIPELINE_SYNC 是 PostgreSQL 返回的同步边界，表示上一条 Sync 之前的
        // 那批 SQL 已经全部处理完，可以统一把结果交给协程了。
        if (status == PGRES_PIPELINE_SYNC)
        {
            CompletePipeline();
            if (connection_failed_)
            {
                return;
            }
            continue;
        }

        // 普通结果按发送顺序对应队首请求；一次可读事件里可以取走多条。
        if (in_flight_requests_.empty())
        {
            FailConnection("PostgreSQL returned a result without a matching request");
            return;
        }

        InFlightRequest request = std::move(in_flight_requests_.front());
        in_flight_requests_.pop_front();

        CompletedRequest completed;
        completed.continuation = request.continuation;
        completed.result = request.result;
        completed.sent_at = request.sent_at;
        completed.value.queue_wait_us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                request.sent_at - request.submitted_at).count());

        // PGRES_PIPELINE_ABORTED：这条 SQL 因为同批次里前面的 SQL 失败被跳过，实际没有执行。
        if (status == PGRES_PIPELINE_ABORTED)
        {
            completed.value.success = false;
            completed.value.value = 0;
            completed.value.error =
                "request was not executed because an earlier SQL statement failed in this pipeline";
            sync_failed_ = true;
            if (sync_failure_message_.empty())
            {
                sync_failure_message_ = completed.value.error;
            }
            completed_requests_.push_back(std::move(completed));
            continue;
        }

        DbResult parsed = ParseResult(result.get(), request.request.operation);
        parsed.queue_wait_us = completed.value.queue_wait_us;
        completed.value = std::move(parsed);

        // PGRES_FATAL_ERROR：这条 SQL 自身执行失败；整批结果要等同步点再统一判定是否回滚。
        if (status == PGRES_FATAL_ERROR)
        {
            sync_failed_ = true;
            if (sync_failure_message_.empty())
            {
                sync_failure_message_ = completed.value.error;
            }
        }
        // PGRES_BAD_RESPONSE：PostgreSQL 返回了无法按协议解析的响应，这条连接不能再继续使用。
        else if (status == PGRES_BAD_RESPONSE)
        {
            FailConnection("PostgreSQL returned a bad pipeline response: " + completed.value.error);
            return;
        }

        completed_requests_.push_back(std::move(completed));
    }
}

void LoopDatabaseContext::CompletePipeline()
{
    // 一个同步点之间的 SQL 属于同一个隐式事务。只有确认收到同步点，才能保证这批写操作
    // 没有被同批的其他失败拖累回滚，所以结果统一在这里交付，而不是结果一回来就交给协程。
    NLOG_DEBUG("[db] CompletePipeline enter completed={} sync_failed={} inflight={} tid={}",
               completed_requests_.size(),
               sync_failed_,
               in_flight_requests_.size(),
               ThreadId());

    if (completed_requests_.empty())
    {
        // 没有待确认的请求却收到同步点，说明连接状态和本地记录已经对不上。
        FailConnection("PostgreSQL pipeline sync arrived without any pending result");
        return;
    }

    const TimePoint completed_at = Clock::now();
    std::vector<std::coroutine_handle<>> continuations;
    continuations.reserve(completed_requests_.size());

    for (CompletedRequest& request : completed_requests_)
    {
        // 一个同步点之间属于同一个隐式事务。只要其中一条 SQL 出错，之前成功执行的写操作也会回滚。
        // 因此不能提前把这一批里的成功结果交给 RPC 协程，必须等同步点确认后再统一完成。
        if (sync_failed_ && request.value.success)
        {
            request.value.success = false;
            request.value.value = 0;
            request.value.error = "pipeline batch was rolled back because another SQL statement failed: " +
                                  sync_failure_message_;
        }

        request.value.database_call_us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                completed_at - request.sent_at).count());
        *request.result = std::move(request.value);
        continuations.push_back(request.continuation);
    }

    completed_requests_.clear();
    sync_failed_ = false;
    sync_failure_message_.clear();

    NLOG_DEBUG("[db] CompletePipeline resume {} continuations tid={}",
               continuations.size(),
               ThreadId());

    for (const std::coroutine_handle<> continuation : continuations)
    {
        ResumeLater(continuation);
    }
}

std::string LoopDatabaseContext::ValidateRequest(const DbRequest& request) const
{
    if (request.key == 0 || request.key > kPostgresBigintMax)
    {
        return "database key must be between 1 and PostgreSQL BIGINT max";
    }

    if (request.operation == DbOperation::Insert && request.value > kPostgresBigintMax)
    {
        return "database value exceeds PostgreSQL BIGINT max";
    }

    switch (request.operation)
    {
        case DbOperation::Read:
        case DbOperation::Insert:
        case DbOperation::Update:
        case DbOperation::Delete:
            return {};
        default:
            return "unsupported database operation";
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

    const TimePoint failed_at = Clock::now();
    std::vector<std::coroutine_handle<>> continuations;
    continuations.reserve(in_flight_requests_.size() + completed_requests_.size());

    // 已经发出去但没等到同步点的请求：SQL 可能已经执行，也可能被回滚，只能报"结果未知"。
    for (InFlightRequest& request : in_flight_requests_)
    {
        DbResult result{false, 0,
                        "PostgreSQL connection failed before the pipeline sync point; transaction outcome is unknown: " +
                            connection_failure_};
        result.queue_wait_us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                request.sent_at - request.submitted_at).count());
        result.database_call_us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                failed_at - request.sent_at).count());
        *request.result = std::move(result);
        continuations.push_back(request.continuation);
    }
    in_flight_requests_.clear();

    // 已经拿到结果但同步点没到：同样不能当作成功。
    for (CompletedRequest& request : completed_requests_)
    {
        DbResult result{false, 0,
                        "PostgreSQL connection failed before the pipeline sync point; transaction outcome is unknown: " +
                            connection_failure_};
        result.queue_wait_us = request.value.queue_wait_us;
        result.database_call_us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                failed_at - request.sent_at).count());
        *request.result = std::move(result);
        continuations.push_back(request.continuation);
    }
    completed_requests_.clear();

    sync_pending_ = false;
    sync_scheduled_ = false;
    sync_failed_ = false;
    sync_failure_message_.clear();

    for (const std::coroutine_handle<> continuation : continuations)
    {
        ResumeLater(continuation);
    }
}

void LoopDatabaseContext::ResumeLater(std::coroutine_handle<> continuation)
{
    if (!continuation)
    {
        return;
    }

    // 不能在这里直接 resume：此刻还在 PostgreSQL socket 的事件回调里，
    // 协程恢复后可能立刻提交新请求，进而改动正在被遍历的队列。
    // 交给 EventLoop 在当前这一轮处理结束后统一恢复，避开回调内重入。
    NLOG_DEBUG("[db] ResumeLater queued cont=0x{:x} tid={}",
               ContinuationAddress(continuation),
               ThreadId());

    loop_->QueueInLoop([continuation]
        {
            NLOG_DEBUG("[db] ResumeLater resume begin cont=0x{:x} tid={}",
                       ContinuationAddress(continuation),
                       ThreadId());
            continuation.resume();
            NLOG_DEBUG("[db] ResumeLater resume end cont=0x{:x} tid={}",
                       ContinuationAddress(continuation),
                       ThreadId());
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

    return TrimTrailingNewlines(message);
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
