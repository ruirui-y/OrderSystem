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
#include <string>

namespace ordersystem::benchmark
{

// 每个 IO EventLoop 独占一条 PostgreSQL 连接。
// 一轮事件处理里收集到的请求共用一个 Pipeline 同步点，结果按发送顺序对应回各自的 RPC 协程。
class LoopDatabaseContext final
{
public:
    LoopDatabaseContext(nebula::net::EventLoop* loop,
                        DbConfig config,
                        std::size_t max_outstanding_requests = 4096);
    ~LoopDatabaseContext();

    LoopDatabaseContext(const LoopDatabaseContext&) = delete;
    LoopDatabaseContext& operator=(const LoopDatabaseContext&) = delete;

    [[nodiscard]] nebula::net::EventLoop* Loop() const noexcept;
    void AssertInOwnerLoop() const;

    // 请求立即发送给 PostgreSQL 后返回，不等待执行结果。
    void Submit(DbRequest request, std::coroutine_handle<> continuation, DbResult* result);

private:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    // 已经发给 PostgreSQL、还没收到结果的请求，按发送顺序排队。
    struct InFlightRequest
    {
        DbRequest request;
        std::coroutine_handle<> continuation;
        DbResult* result{nullptr};
        TimePoint submitted_at;
        TimePoint sent_at;
    };

    // 结果已经收到并解析，但还没等到 Pipeline 同步点的请求。
    struct CompletedRequest
    {
        std::coroutine_handle<> continuation;
        DbResult* result{nullptr};
        DbResult value;
        TimePoint sent_at;
    };

    void ConnectAndPrepare();
    void Prepare(std::size_t index, const char* name, const char* sql, int parameter_count);

    bool SendInFlightRequest(InFlightRequest& request);
    void SchedulePipelineSync();
    void SendPipelineSync();
    bool FlushOutput();
    void OnReadable();
    void OnWritable();
    void OnSocketClosed();
    void OnSocketError();
    void ProcessResults();
    void CompletePipeline();

    [[nodiscard]] std::string ValidateRequest(const DbRequest& request) const;
    DbResult ParseResult(PGresult* result, DbOperation operation) const;
    void FailConnection(std::string error);
    void ResumeLater(std::coroutine_handle<> continuation);
    void Close() noexcept;

    [[nodiscard]] std::string ConnectionError() const;

    nebula::net::EventLoop* loop_;
    DbConfig config_;
    const std::size_t max_outstanding_requests_;

    PGconn* connection_{nullptr};
    std::unique_ptr<nebula::net::Channel> channel_;
    std::array<const char*, 4> statement_names_{};

    std::deque<InFlightRequest> in_flight_requests_;                                // 已发送、等结果的请求，按发送顺序排队；结果回来时从队首依次取走。
    std::deque<CompletedRequest> completed_requests_;                               // 已出结果、等同步点确认的请求；同步点之前不能交给协程。
    bool sync_pending_{false};                                                      // 有请求已经发出，但还没被同步点覆盖
    bool sync_scheduled_{false};                                                    // 本轮已经安排过一次同步点
    bool sync_failed_{false};                                                       // 当前同步点之前有 SQL 执行失败
    std::string sync_failure_message_;                                              // 第一条失败 SQL 的错误，用于回滚整批结果

    bool connection_failed_{false};
    std::string connection_failure_;
};

// RPC 协程通过这个 Awaiter 把请求交给所属 EventLoop 的数据库上下文。
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
