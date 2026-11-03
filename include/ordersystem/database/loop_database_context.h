#pragma once

#include "ordersystem/database/database_types.h"

#include "nebula/net/channel.h"
#include "nebula/net/event_loop.h"

#include <libpq-fe.h>

#include <array>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <deque>
#include <memory>
#include <optional>
#include <string>

namespace ordersystem::benchmark
{

// 阶段 2 中每个 IO EventLoop 独占一条 PostgreSQL 连接。
// 连接的 socket 直接注册到同一个 EventLoop，数据库返回数据时由 Channel 回调继续处理。
// 当前仍保持“一条连接一次只执行一个 SQL”，后续阶段再引入 Pipeline。
class LoopDatabaseContext final
{
public:
    LoopDatabaseContext(nebula::net::EventLoop* loop, DbConfig config);
    ~LoopDatabaseContext();

    LoopDatabaseContext(const LoopDatabaseContext&) = delete;
    LoopDatabaseContext& operator=(const LoopDatabaseContext&) = delete;

    [[nodiscard]] nebula::net::EventLoop* Loop() const noexcept;
    void AssertInOwnerLoop() const;

    // 提交请求后立即返回，不等待 PostgreSQL。
    // result 指向挂起协程帧中的结果对象，在 continuation 恢复前一直有效。
    void Submit(DbRequest request, std::coroutine_handle<> continuation, DbResult* result);

private:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    struct PendingRequest
    {
        DbRequest request;
        std::coroutine_handle<> continuation;
        DbResult* result{nullptr};
        TimePoint submitted_at;
    };

    void ConnectAndPrepare();
    void Prepare(std::size_t index, const char* name, const char* sql, int parameter_count);

    void StartNextRequest();                                                        // 当前连接空闲时，从本 Loop 的等待队列取一个请求并异步发送。
    bool SendActiveRequest();
    bool FlushOutput();                                                             // libpq 还有数据没写完时临时监听 EPOLLOUT，不在这里阻塞等待。
    void OnReadable();
    void OnWritable();
    void OnSocketClosed();
    void OnSocketError();
    void ProcessResults();                                                          // 只在 PQisBusy()==0 时调用 PQgetResult，保证不会阻塞 EventLoop。

    DbResult ParseResult(PGresult* result, DbOperation operation) const;
    void CompleteActive(DbResult result);
    void FailConnection(std::string error);
    void ResumeLater(std::coroutine_handle<> continuation);
    void Close() noexcept;

    [[nodiscard]] std::string ConnectionError() const;

    nebula::net::EventLoop* loop_;
    DbConfig config_;

    PGconn* connection_{nullptr};
    std::unique_ptr<nebula::net::Channel> channel_;
    std::array<const char*, 4> statement_names_{};

    std::deque<PendingRequest> pending_requests_;                                   // 同一个 EventLoop 上等待这条 PostgreSQL 连接的请求
    std::optional<PendingRequest> active_request_;                                  // 当前已经发送、正在等待 PostgreSQL 结果的请求
    std::optional<DbResult> active_result_;                                         // 已收到的 PGresult 先保存在这里，等 PQgetResult 返回 nullptr 后再完成请求
    TimePoint active_started_at_{};

    bool connection_failed_{false};
    std::string connection_failure_;
};

// RPC 协程通过这个 Awaiter 把数据库请求交给当前 EventLoop 的数据库上下文。
// SQL 完成前协程保持挂起，结果由 PostgreSQL socket 的可读事件驱动回来。
class DbOperationAwaiter final
{
public:
    DbOperationAwaiter(LoopDatabaseContext& database, DbRequest request);

    [[nodiscard]] bool await_ready() const noexcept;
    void await_suspend(std::coroutine_handle<> continuation);
    DbResult await_resume() noexcept;

private:
    LoopDatabaseContext& database_;
    DbRequest request_;
    DbResult result_;
};

}  // namespace ordersystem::benchmark
