// NebulaRPC Fixed-InFlight 协程压测客户端。
//
// 测试模型：
//   workers = OS Thread = EventLoop = RpcChannel = TCP Connection
//   window  = 每条 TCP 连接上同时维持的最大 in-flight RPC 数
//   total_inflight = workers * window
//
// 每个 Lane 是一条串行 RPC 链；同一 Worker 上的 window 个 Lane 共享一条 RpcChannel。
// 初始阶段先让全部 Lane 运行到第一个 co_await，把 window 个请求写入 output_buffer，
// 然后统一 Flush。后续响应到达时，RpcChannel 会先恢复本批协程，再统一 Flush 下一批请求。

#include "nebula/net/event_loop.h"
#include "nebula/rpc/rpc_channel.h"
#include "nebula/rpc/rpc_coroutine.h"
#include "order_db_benchmark.pb.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;             // 用来测量耗时的时钟，不会因为系统校时而倒退
using TimePoint = Clock::time_point;                 // 时间轴上的具体时刻，不是“还要等多久”
using DbBenchmarkRequest = ordersystem::benchmark::DbBenchmarkRequest;
using DbBenchmarkResponse = ordersystem::benchmark::DbBenchmarkResponse;
using DbCoroutineStub = nebula::rpc::CoroutineStub<ordersystem::benchmark::DbBenchmarkService>;
constexpr std::uint64_t kSeedRowCount = 1000000ULL;  // READ/UPDATE/DELETE 预先准备的记录数

struct Options
{
    std::string host = "127.0.0.1";
    std::uint16_t port = 9000;

    std::size_t workers = 4;           // 客户端启动多少个工作线程，也就是多少条 RPC 连接
    std::size_t window = 16;           // 每条 RPC 连接最多同时等待响应的请求数

    std::uint64_t requests = 1000000;  // 整个客户端本轮要完成的请求总数，再分给各个 Worker
    std::string operation = "read";    // 本轮只测一种操作：read/insert/update/delete

    int timeout_ms = 2000;             // 单次 RPC 最多等待多少毫秒
    int connect_timeout_ms = 10000;    // 等待 RPC 连接建立的最长时间（毫秒）
    int max_seconds = 120;             // 整轮测试允许运行的最长时间，超时就停止

    std::string csv;                   // 结果 CSV 的保存路径，留空则不写文件
};

[[noreturn]] void Usage(const char* exe, const std::string& error = {})
{
    if (!error.empty())
    {
        std::cerr << "Invalid option: " << error << '\n';
    }

    std::cerr
        << "Usage: " << exe
        << " [--host IP] [--port N]"
        << " [--workers N] [--window N]"
        << " [--requests N] [--operation read|insert|update|delete]"
        << " [--timeout-ms N]"
        << " [--connect-timeout-ms N] [--max-seconds N]"
        << " [--csv output.csv]\n\n"
        << "Fixed-InFlight model:\n"
        << "  workers        = OS threads = EventLoops = RpcChannels = TCP connections\n"
        << "  window         = max in-flight RPCs per TCP connection\n"
        << "  total_inflight = workers * window\n\n"
        << "Recommended baseline:\n"
        << "  --workers 4 --window 512 --requests 3000000 --operation read\n";

    std::exit(error.empty() ? 0 : 2);
}

std::uint64_t ParseUnsigned(const std::string& text)
{
    if (text.empty() || text.front() == '-')
    {
        throw std::invalid_argument(text);
    }

    std::size_t used = 0;                                      // 记录数字文本实际读取了多少个字符
    const std::uint64_t value = std::stoull(text, &used, 10);  // 把命令行中的数字文本转成整数

    if (used != text.size())
    {
        throw std::invalid_argument(text);
    }

    return value;
}

Options ParseOptions(int argc, char** argv)
{
    Options o;

    for (int i = 1; i < argc; ++i)
    {
        const std::string name(argv[i]);     // 当前参数名，例如 --window

        if (name == "--help" || name == "-h")
        {
            Usage(argv[0]);
        }

        if (i + 1 >= argc)
        {
            Usage(argv[0], name + " has no value");
        }

        const std::string value(argv[++i]);  // 当前参数名后面跟着的值

        try
        {
            if (name == "--host")
            {
                o.host = value;
            }
            else if (name == "--port")
            {
                const auto n = ParseUnsigned(value);
                if (n == 0 || n > 65535)
                {
                    Usage(argv[0], "port must be in [1, 65535]");
                }
                o.port = static_cast<std::uint16_t>(n);
            }
            else if (name == "--workers")
            {
                o.workers = static_cast<std::size_t>(ParseUnsigned(value));
            }
            else if (name == "--window")
            {
                o.window = static_cast<std::size_t>(ParseUnsigned(value));
            }
            else if (name == "--requests")
            {
                o.requests = ParseUnsigned(value);
            }
            else if (name == "--operation")
            {
                o.operation = value;
            }
            else if (name == "--timeout-ms")
            {
                o.timeout_ms = static_cast<int>(ParseUnsigned(value));
            }
            else if (name == "--connect-timeout-ms")
            {
                o.connect_timeout_ms = static_cast<int>(ParseUnsigned(value));
            }
            else if (name == "--max-seconds")
            {
                o.max_seconds = static_cast<int>(ParseUnsigned(value));
            }
            else if (name == "--csv")
            {
                o.csv = value;
            }
            else
            {
                Usage(argv[0], "unknown option: " + name);
            }
        }
        catch (const std::exception&)
        {
            Usage(argv[0], name + "=" + value);
        }
    }

    if (o.host.empty() ||
        o.workers == 0 || o.workers > 128 ||
        o.window == 0 || o.window > 65536 ||
        o.requests < o.workers || o.requests > 100000000ULL ||
        (o.operation != "read" && o.operation != "insert" &&
         o.operation != "update" && o.operation != "delete") ||
        (o.operation == "delete" && o.requests > kSeedRowCount) ||
        o.timeout_ms < 1 || o.timeout_ms > 120000 ||
        o.connect_timeout_ms < 1 || o.connect_timeout_ms > 120000 ||
        o.max_seconds < 1 || o.max_seconds > 86400)
    {
        Usage(argv[0], "configuration outside supported range");
    }

    return o;
}

struct SharedStart
{
    std::atomic<std::size_t> ready{0};  // 已经连好 RPC 或已报告启动失败的 Worker 数量
    std::atomic<bool> go{false};        // 主线程发出开始信号后，各个 Worker 才进入正式压测
    std::atomic<bool> cancel{false};    // 启动失败时通知其他 Worker 一起取消启动
};

struct Stats
{
    std::size_t worker_id = 0;                    // 这份统计属于哪个 Worker，从 0 开始编号

    std::uint64_t issued = 0;                     // 已经发出多少个 RPC 请求
    std::uint64_t completed = 0;                  // 已经收到响应的请求数，不代表业务一定成功
    std::uint64_t succeeded = 0;                  // 数据库操作成功的请求数
    std::uint64_t failed = 0;                     // RPC 失败或数据库操作失败的请求数
    std::uint64_t timed_out = 0;                  // 等待 RPC 响应超过限定时间的次数
    std::uint64_t cancelled = 0;                  // 被取消的 RPC 请求数
    std::uint64_t mismatched = 0;                 // 响应内容与预期不符的次数（当前代码没有单独校验这个项）
    std::uint64_t unfinished = 0;                 // 测试结束时，已发出但还没收到响应的请求数

    TimePoint started{};                          // 这个 Worker 进入正式测试的时刻
    TimePoint stopped{};                          // 这个 Worker 结束测试的时刻

    std::vector<std::int64_t> latency_ns;         // 成功请求的完整 RPC 耗时，单位是纳秒
    std::vector<std::uint32_t> database_call_us;  // 服务端记录的数据库调用耗时，单位是微秒
    std::vector<std::uint32_t> db_queue_wait_us;  // 请求在数据库线程池中等待的时间，单位是微秒
    std::string error;                            // Worker 启动或压测流程中的错误
    std::string db_error;                         // 数据库操作失败时服务端返回的错误信息
};

DbBenchmarkRequest::Operation OperationFromName(const std::string& operation)
{
    if (operation == "read")
    {
        return DbBenchmarkRequest::READ;
    }
    if (operation == "insert")
    {
        return DbBenchmarkRequest::INSERT;
    }
    if (operation == "update")
    {
        return DbBenchmarkRequest::UPDATE;
    }
    return DbBenchmarkRequest::DELETE;
}

class Worker
{
public:
    Worker(const Options& opts,
           SharedStart& shared,
           std::size_t worker_id,
           std::uint64_t request_count,
           std::uint64_t base_sequence)
        : opts_(opts),
          shared_(shared),
          target_(request_count),
          worker_id_(worker_id),
          base_sequence_(base_sequence),
          channel_(&loop_, opts.host, opts.port),
          stub_(&channel_),
          method_(stub_.ResolveMethod("Execute"))
    {
        stats_.worker_id = worker_id;
        stats_.latency_ns.reserve(static_cast<std::size_t>(target_));
        stats_.database_call_us.reserve(static_cast<std::size_t>(target_));
        stats_.db_queue_wait_us.reserve(static_cast<std::size_t>(target_));
    }

    Stats Run()
    {
        setup_task_ = Setup();
        setup_task_.Start();

        loop_.Loop();

        CollectTaskErrors();
        stats_.unfinished = stats_.issued - stats_.completed;
        return std::move(stats_);
    }

private:
    void AnnounceReady()
    {
        if (announced_ready_)
        {
            return;
        }

        announced_ready_ = true;
        shared_.ready.fetch_add(1, std::memory_order_release);
    }

    void Abort(const std::string& reason)
    {
        if (stats_.error.empty())
        {
            stats_.error = reason;
        }

        AnnounceReady();

        if (started_ && stats_.stopped == TimePoint{})
        {
            stats_.stopped = Clock::now();
        }

        loop_.Quit();
    }

    nebula::coro::Task<int> Setup()
    {
        const TimePoint connect_deadline = Clock::now() +
            std::chrono::milliseconds(opts_.connect_timeout_ms);  // 连接必须在这个时刻之前建立成功

        while (!channel_.Connected())
        {
            if (shared_.cancel.load(std::memory_order_acquire))
            {
                Abort("benchmark start cancelled while connecting");
                co_return 1;
            }

            if (Clock::now() >= connect_deadline)
            {
                Abort("RPC connection timeout");
                co_return 1;
            }

            co_await nebula::net::SleepFor(&loop_, std::chrono::milliseconds(10));
        }

        AnnounceReady();                                          // 不发送预热请求，避免 INSERT/DELETE 提前改变测试表内容

        while (!shared_.go.load(std::memory_order_acquire))
        {
            if (shared_.cancel.load(std::memory_order_acquire))
            {
                Abort("benchmark start cancelled before measured phase");
                co_return 1;
            }

            co_await nebula::net::SleepFor(&loop_, std::chrono::milliseconds(1));
        }

        if (shared_.cancel.load(std::memory_order_acquire))
        {
            Abort("benchmark start cancelled");
            co_return 1;
        }

        started_ = true;
        stats_.started = Clock::now();

        loop_.RunAfter(std::chrono::seconds(opts_.max_seconds), [this]
            {
                if (active_lanes_ != 0)
                {
                    Abort("measured phase exceeded --max-seconds");
                }
            });

        StartLanes();
        co_return 0;
    }

    nebula::coro::Task<int> RunLane()                                // 每个 Lane 最多有 1 个 RPC 在途；window 个 Lane 共用一条连接
    {
        while (stats_.error.empty())
        {
            if (next_request_ >= target_)
            {
                break;
            }

            const std::uint64_t sequence = next_request_++;          // 当前 Worker 内这是第几条请求，从 0 开始计数

            DbBenchmarkRequest request;
            request.set_operation(OperationFromName(opts_.operation));
            request.set_value(sequence + 1);

            if (opts_.operation == "insert" || opts_.operation == "delete")
            {
                request.set_key(base_sequence_ + sequence + 1);      // INSERT/DELETE 每次使用不同 key
            }
            else
            {
                const std::uint64_t global_sequence =
                    sequence * static_cast<std::uint64_t>(opts_.workers) +
                    static_cast<std::uint64_t>(worker_id_);          // 把各 Worker 的请求合成一个全局序号
                const std::uint64_t mixed_key =
                    global_sequence * std::uint64_t{2654435761ULL};  // 根据请求序号算出预置表中的记录编号
                request.set_key(mixed_key % kSeedRowCount + 1);      // READ/UPDATE 分散访问预置数据，避免集中争抢同一行
            }

            nebula::rpc::RpcCallOptions options;
            options.timeout = std::chrono::milliseconds(opts_.timeout_ms);
            options.flush = false;                                   // 先把请求放进发送缓冲区，等本轮 Lane 都启动后再统一 Flush

            const TimePoint sent_at = Clock::now();                  // 发出这条 RPC 前的时刻，用来计算往返耗时
            ++stats_.issued;
            ++in_flight_;

            auto result = co_await stub_.Call<DbBenchmarkRequest, DbBenchmarkResponse>(
                method_, std::move(request), options);

            const TimePoint ended_at = Clock::now();                 // 收到这条 RPC 响应后的时刻

            --in_flight_;
            ++stats_.completed;

            if (!result)
            {
                ++stats_.failed;

                if (result.State() == nebula::rpc::RpcCallState::Timeout)
                {
                    ++stats_.timed_out;
                }
                else if (result.State() == nebula::rpc::RpcCallState::Cancelled)
                {
                    ++stats_.cancelled;
                }

                continue;
            }

            stats_.database_call_us.push_back(
                static_cast<std::uint32_t>(result.ResponseValue().database_call_us()));
            stats_.db_queue_wait_us.push_back(
                static_cast<std::uint32_t>(result.ResponseValue().db_queue_wait_us()));

            if (!result.ResponseValue().success())
            {
                ++stats_.failed;
                if (stats_.db_error.empty())
                {
                    stats_.db_error = result.ResponseValue().error();
                }
                continue;
            }

            ++stats_.succeeded;
            stats_.latency_ns.push_back(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    ended_at - sent_at).count());
        }

        OnLaneFinished();
        co_return stats_.error.empty() ? 0 : 1;
    }

    void StartLanes()
    {
        const std::size_t lane_count = static_cast<std::size_t>(
            std::min<std::uint64_t>(
                static_cast<std::uint64_t>(opts_.window),
                target_));  // 实际启动的 Lane 数，不超过 window 和本 Worker 的剩余请求数

        if (lane_count == 0)
        {
            stats_.stopped = Clock::now();
            loop_.Quit();
            return;
        }

        lane_tasks_.reserve(lane_count);
        active_lanes_ = lane_count;

        for (std::size_t i = 0; i < lane_count; ++i)
        {
            lane_tasks_.emplace_back(RunLane());
        }

        for (auto& task : lane_tasks_)
        {
            task.Start();
        }

        stub_.Flush();      // 所有 Lane 都已启动，现在一次性发送第一批请求
    }

    void OnLaneFinished()
    {
        if (active_lanes_ == 0)
        {
            return;
        }

        --active_lanes_;

        if (active_lanes_ == 0)
        {
            stats_.stopped = Clock::now();
            loop_.Quit();
        }
    }

    void CollectTaskErrors()
    {
        if (setup_task_.Done())
        {
            try
            {
                if (setup_task_.Result() != 0 && stats_.error.empty())
                {
                    stats_.error = "setup coroutine returned failure";
                }
            }
            catch (const std::exception& error)
            {
                if (stats_.error.empty())
                {
                    stats_.error = error.what();
                }
            }
        }

        for (auto& task : lane_tasks_)
        {
            if (!task.Done())
            {
                continue;
            }

            try
            {
                if (task.Result() != 0 && stats_.error.empty())
                {
                    stats_.error = "lane coroutine returned failure";
                }
            }
            catch (const std::exception& error)
            {
                if (stats_.error.empty())
                {
                    stats_.error = error.what();
                }
            }
        }
    }

    const Options& opts_;                                         // 本次压测的配置
    SharedStart& shared_;                                         // 与其他 Worker 共用的启动信号
    std::uint64_t target_ = 0;                                    // 分配给这个 Worker 的请求总数
    std::size_t worker_id_ = 0;                                   // 当前 Worker 的编号，从 0 开始
    std::uint64_t base_sequence_ = 0;                             // INSERT/DELETE 使用的起始编号，避免不同 Worker 生成相同 key

    nebula::net::EventLoop loop_;                                 // 这个 Worker 的事件循环，负责驱动协程和 RPC 回调
    nebula::rpc::RpcChannel channel_;                             // 这个 Worker 自己的 RPC/TCP 连接
    DbCoroutineStub stub_;                                        // 通过 channel 发送数据库压测 RPC 的对象
    const google::protobuf::MethodDescriptor* method_ = nullptr;  // 服务端 Execute 方法的描述信息

    std::uint64_t next_request_ = 0;                              // 下一条要发送的请求序号
    std::size_t in_flight_ = 0;                                   // 当前已发出、还没收到响应的 RPC 数量
    std::size_t active_lanes_ = 0;                                // 还没有完成的 Lane 数量

    bool announced_ready_ = false;                                // 是否已经报告准备完成，防止 ready 被重复加一
    bool started_ = false;                                        // 这个 Worker 是否已经进入正式测试

    Stats stats_;

    nebula::coro::Task<int> setup_task_;                          // 负责连接和启动 Lane；它会先于 channel_ 析构
    std::vector<nebula::coro::Task<int>> lane_tasks_;             // 保存这个 Worker 启动的所有 Lane 协程
};

template <typename T>
T Percentile(const std::vector<T>& sorted, double percent)
{
    if (sorted.empty())
    {
        return 0;
    }

    const std::size_t index = std::min(
        sorted.size() - 1,
        static_cast<std::size_t>(
            std::ceil(percent * static_cast<double>(sorted.size()))) - 1);  // 排序后样本中该百分位的位置，下标从 0 开始

    return sorted[index];
}

int Report(const Options& o, std::vector<Stats>& results)
{
    std::vector<std::int64_t> latencies;                  // 合并所有 Worker 的 RPC 耗时样本
    std::vector<std::uint32_t> database_call_samples_us;  // 合并所有 Worker 的数据库调用耗时样本（微秒）
    std::vector<std::uint32_t> db_queue_wait_samples_us;  // 合并所有 Worker 的数据库排队耗时样本（微秒）

    std::uint64_t issued = 0;
    std::uint64_t completed = 0;
    std::uint64_t succeeded = 0;
    std::uint64_t failed = 0;
    std::uint64_t timed_out = 0;
    std::uint64_t cancelled = 0;
    std::uint64_t mismatched = 0;
    std::uint64_t unfinished = 0;

    TimePoint earliest_start{};  // 所有 Worker 中最早进入测试的时刻
    TimePoint latest_stop{};     // 最后一个 Worker 结束测试的时刻
    bool bad = false;            // 只要任何 Worker 出错，本轮就不能算作完整成功

    for (const auto& s : results)
    {
        issued += s.issued;
        completed += s.completed;
        succeeded += s.succeeded;
        failed += s.failed;
        timed_out += s.timed_out;
        cancelled += s.cancelled;
        mismatched += s.mismatched;
        unfinished += s.unfinished;

        if (!s.error.empty())
        {
            std::cerr << "worker " << s.worker_id << ": " << s.error << '\n';
            bad = true;
        }
        if (!s.db_error.empty())
        {
            std::cerr << "worker " << s.worker_id << " database error: " << s.db_error << '\n';
            bad = true;
        }

        if (s.started != TimePoint{} &&
            (earliest_start == TimePoint{} || s.started < earliest_start))
        {
            earliest_start = s.started;
        }

        if (s.stopped != TimePoint{} &&
            (latest_stop == TimePoint{} || s.stopped > latest_stop))
        {
            latest_stop = s.stopped;
        }

        latencies.insert(
            latencies.end(),
            s.latency_ns.begin(),
            s.latency_ns.end());
        database_call_samples_us.insert(
            database_call_samples_us.end(),
            s.database_call_us.begin(),
            s.database_call_us.end());
        db_queue_wait_samples_us.insert(
            db_queue_wait_samples_us.end(),
            s.db_queue_wait_us.begin(),
            s.db_queue_wait_us.end());
    }

    std::sort(latencies.begin(), latencies.end());
    std::sort(database_call_samples_us.begin(), database_call_samples_us.end());
    std::sort(db_queue_wait_samples_us.begin(), db_queue_wait_samples_us.end());

    const double wall_seconds =
        earliest_start != TimePoint{} && latest_stop > earliest_start
        ? std::chrono::duration<double>(latest_stop - earliest_start).count()
        : 0.0;                                 // 整轮测试耗时，从最早开始到最晚结束，单位秒

    const double success_qps = wall_seconds > 0.0
        ? static_cast<double>(succeeded) / wall_seconds
        : 0.0;                                 // 每秒成功完成的请求数

    const double avg_us = !latencies.empty()
        ? std::accumulate(latencies.begin(), latencies.end(), 0.0) /
            static_cast<double>(latencies.size()) / 1000.0
        : 0.0;                                 // 成功 RPC 的平均往返耗时，单位微秒

    const auto ToUs = [](std::int64_t ns)
        {
            return static_cast<double>(ns) / 1000.0;
        };

    const double p50_us = ToUs(Percentile(latencies, 0.50));
    const double p95_us = ToUs(Percentile(latencies, 0.95));
    const double p99_us = ToUs(Percentile(latencies, 0.99));
    const double p999_us = ToUs(Percentile(latencies, 0.999));
    const double database_call_avg_us = !database_call_samples_us.empty()
        ? std::accumulate(database_call_samples_us.begin(), database_call_samples_us.end(), 0.0) /
            static_cast<double>(database_call_samples_us.size())
        : 0.0;
    const double database_call_p50_us = static_cast<double>(Percentile(database_call_samples_us, 0.50));
    const double database_call_p95_us = static_cast<double>(Percentile(database_call_samples_us, 0.95));
    const double database_call_p99_us = static_cast<double>(Percentile(database_call_samples_us, 0.99));
    const double database_call_p999_us = static_cast<double>(Percentile(database_call_samples_us, 0.999));
    const double db_queue_wait_avg_us = !db_queue_wait_samples_us.empty()
        ? std::accumulate(db_queue_wait_samples_us.begin(), db_queue_wait_samples_us.end(), 0.0) /
            static_cast<double>(db_queue_wait_samples_us.size())
        : 0.0;
    const double db_queue_wait_p50_us = static_cast<double>(Percentile(db_queue_wait_samples_us, 0.50));
    const double db_queue_wait_p95_us = static_cast<double>(Percentile(db_queue_wait_samples_us, 0.95));
    const double db_queue_wait_p99_us = static_cast<double>(Percentile(db_queue_wait_samples_us, 0.99));
    const double db_queue_wait_p999_us = static_cast<double>(Percentile(db_queue_wait_samples_us, 0.999));

    const std::uint64_t total_inflight =
        static_cast<std::uint64_t>(o.workers) *
        static_cast<std::uint64_t>(o.window);  // 所有 Worker 合起来最多同时等待响应的 RPC 数量

    std::cout << std::fixed << std::setprecision(3)
        << "mode=closed_loop_fixed_inflight"
        << " framework=NebulaRPC"
        << " host=" << o.host << ':' << o.port
        << " workers=" << o.workers
        << " connections=" << o.workers
        << " window=" << o.window
        << " total_inflight=" << total_inflight
        << " operation=" << o.operation << '\n'
        << "target=" << o.requests
        << " issued=" << issued
        << " completed=" << completed
        << " success=" << succeeded
        << " failed=" << failed
        << " timeout=" << timed_out
        << " cancelled=" << cancelled
        << " mismatched=" << mismatched
        << " unfinished=" << unfinished << '\n'
        << "wall_seconds=" << wall_seconds
        << " success_qps=" << success_qps << '\n'
        << "latency_success_only_us: avg=" << avg_us
        << " p50=" << p50_us
        << " p95=" << p95_us
        << " p99=" << p99_us
        << " p999=" << p999_us << '\n'
        << "database_call_us: avg=" << database_call_avg_us
        << " p50=" << database_call_p50_us
        << " p95=" << database_call_p95_us
        << " p99=" << database_call_p99_us
        << " p999=" << database_call_p999_us << '\n'
        << "db_queue_wait_us: avg=" << db_queue_wait_avg_us
        << " p50=" << db_queue_wait_p50_us
        << " p95=" << db_queue_wait_p95_us
        << " p99=" << db_queue_wait_p99_us
        << " p999=" << db_queue_wait_p999_us << '\n';

    if (!o.csv.empty())
    {
        std::ifstream old(o.csv, std::ios::binary | std::ios::ate);
        const bool write_header = !old.good() || old.tellg() == std::streampos(0);  // CSV 是新文件或空文件时，先写列名

        std::ofstream out(o.csv, std::ios::app);
        if (!out)
        {
            throw std::runtime_error("unable to open csv: " + o.csv);
        }

        if (write_header)
        {
            out
                << "framework,mode,operation,host,port,workers,connections,window,total_inflight,"
                << "target,issued,completed,success,failed,timeout,"
                << "cancelled,mismatch,unfinished,wall_seconds,success_qps,"
                << "avg_us,p50_us,p95_us,p99_us,p999_us,"
                << "database_call_avg_us,database_call_p50_us,database_call_p95_us,"
                << "database_call_p99_us,database_call_p999_us,"
                << "db_queue_wait_avg_us,db_queue_wait_p50_us,db_queue_wait_p95_us,"
                << "db_queue_wait_p99_us,db_queue_wait_p999_us\n";
        }

        out << std::fixed << std::setprecision(6)
            << "NebulaRPC,closed_loop_fixed_inflight," << o.operation << ','
            << o.host << ',' << o.port << ','
            << o.workers << ',' << o.workers << ','
            << o.window << ',' << total_inflight << ','
            << o.requests << ',' << issued << ',' << completed << ',' << succeeded << ','
            << failed << ',' << timed_out << ',' << cancelled << ','
            << mismatched << ',' << unfinished << ','
            << wall_seconds << ',' << success_qps << ','
            << avg_us << ',' << p50_us << ',' << p95_us << ','
            << p99_us << ',' << p999_us << ','
            << database_call_avg_us << ',' << database_call_p50_us << ','
            << database_call_p95_us << ',' << database_call_p99_us << ','
            << database_call_p999_us << ','
            << db_queue_wait_avg_us << ',' << db_queue_wait_p50_us << ','
            << db_queue_wait_p95_us << ',' << db_queue_wait_p99_us << ','
            << db_queue_wait_p999_us << '\n';
    }

    if (bad ||
        issued != o.requests ||
        completed != issued ||
        succeeded != o.requests ||
        failed != 0 ||
        timed_out != 0 ||
        cancelled != 0 ||
        mismatched != 0 ||
        unfinished != 0)
    {
        return 1;
    }

    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    try
    {
        const Options options = ParseOptions(argc, argv);                                                // 读取本次压测的命令行配置

        SharedStart shared;
        std::vector<Stats> results(options.workers);                                                     // 每个 Worker 各保存一份统计结果
        std::vector<std::thread> threads;                                                                // 保存所有 Worker 线程，最后逐个 join
        threads.reserve(options.workers);

        for (std::size_t i = 0; i < options.workers; ++i)
        {
            const std::uint64_t per_worker = options.requests / options.workers;                         // 每个 Worker 先分到的基础请求数
            const std::uint64_t remainder = options.requests % options.workers;                          // 除不尽的请求数，依次多分给前面的 Worker
            const std::uint64_t count = per_worker + (i < remainder ? 1ULL : 0ULL);                      // 这个 Worker 最终要发送的请求数
            const std::uint64_t base_sequence = per_worker * i + std::min<std::uint64_t>(i, remainder);  // 给每个 Worker 分配不重叠的 key 编号范围

            threads.emplace_back([&, i, count, base_sequence]
                {
                    try
                    {
                        Worker worker(options, shared, i, count, base_sequence);
                        results[i] = worker.Run();
                    }
                    catch (const std::exception& error)
                    {
                        results[i].worker_id = i;
                        results[i].error = error.what();
                        shared.ready.fetch_add(1, std::memory_order_release);
                    }
                });
        }

        const TimePoint ready_deadline = Clock::now() +
            std::chrono::milliseconds(options.connect_timeout_ms) +
            std::chrono::seconds(30);  // 等待所有 Worker 建立连接并报告准备状态的截止时刻

        while (shared.ready.load(std::memory_order_acquire) < options.workers &&
               Clock::now() < ready_deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        if (shared.ready.load(std::memory_order_acquire) < options.workers)
        {
            std::cerr << "benchmark workers did not become ready before deadline\n";
            shared.cancel.store(true, std::memory_order_release);
        }

        shared.go.store(true, std::memory_order_release);

        for (auto& thread : threads)
        {
            thread.join();
        }

        return Report(options, results);
    }
    catch (const std::exception& error)
    {
        std::cerr << "benchmark error: " << error.what() << '\n';
        return 2;
    }
}
