#include "ordersystem/database/loop_database_context.h"

#include "nebula/base/logger.h"
#include "nebula/net/event_loop.h"
#include "nebula/rpc/rpc_coroutine.h"
#include "nebula/rpc/rpc_server.h"
#include "order_db_benchmark.pb.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{

using ordersystem::benchmark::DbConfig;
using ordersystem::benchmark::DbOperation;
using ordersystem::benchmark::DbOperationAwaiter;
using ordersystem::benchmark::DbRequest;
using ordersystem::benchmark::DbResult;
using ordersystem::benchmark::LoopDatabaseContext;
using Request = ordersystem::benchmark::DbBenchmarkRequest;
using Response = ordersystem::benchmark::DbBenchmarkResponse;

std::string EnvironmentValue(const char* name, std::string fallback)
{
    const char* value = std::getenv(name);
    return value == nullptr ? std::move(fallback) : std::string(value);
}

std::uint64_t ParseNumber(const char* text, const char* name)
{
    try
    {
        std::size_t used = 0;
        const std::string value(text);
        const unsigned long long parsed = std::stoull(value, &used, 10);
        if (used != value.size())
        {
            throw std::invalid_argument("extra characters");
        }
        return static_cast<std::uint64_t>(parsed);
    }
    catch (const std::exception&)
    {
        throw std::invalid_argument(std::string("invalid ") + name + ": " + text);
    }
}

DbConfig ReadDbConfig()
{
    DbConfig config;
    config.host = EnvironmentValue("DB_HOST", "127.0.0.1");
    config.user = EnvironmentValue("DB_USER", "ordersystem_bench");
    config.password = EnvironmentValue("DB_PASSWORD", "");
    config.database = EnvironmentValue("DB_NAME", "ordersystem_bench");

    const std::uint64_t port = ParseNumber(EnvironmentValue("DB_PORT", "5432").c_str(), "DB_PORT");
    if (port == 0 || port > 65535)
    {
        throw std::invalid_argument("DB_PORT must be between 1 and 65535");
    }
    config.port = static_cast<std::uint16_t>(port);
    return config;
}

DbRequest ConvertRequest(const Request& request)
{
    DbRequest result;
    result.key = request.key();
    result.value = request.value();

    switch (request.operation())
    {
        case Request::READ:
            result.operation = DbOperation::Read;
            break;
        case Request::INSERT:
            result.operation = DbOperation::Insert;
            break;
        case Request::UPDATE:
            result.operation = DbOperation::Update;
            break;
        case Request::DELETE:
            result.operation = DbOperation::Delete;
            break;
        default:
            throw std::invalid_argument("unsupported database operation");
    }

    if (result.key == 0 || result.key > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    {
        throw std::invalid_argument("database key must be between 1 and PostgreSQL BIGINT max");
    }
    if (result.operation == DbOperation::Insert &&
        result.value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    {
        throw std::invalid_argument("database value exceeds PostgreSQL BIGINT max");
    }
    return result;
}

}  // namespace

// 阶段 3：每个 IO EventLoop 自己持有一条 PostgreSQL 连接，并通过 Pipeline 批量发送 SQL。
// RPC 请求仍由所属 EventLoop 驱动，不再经过 DB Worker 线程。
int main(int argc, char** argv)
{
    // 诊断运行结束后恢复 INFO 级别；DEBUG 日志只用于排查数据库链路。
    nebula::base::InitLogger(spdlog::level::info);

    try
    {
        const std::uint64_t port_value = argc > 1 ? ParseNumber(argv[1], "port") : 9000;
        const std::uint64_t io_threads_value = argc > 2 ? ParseNumber(argv[2], "io_threads") : 4;

        if (port_value == 0 || port_value > 65535 || io_threads_value == 0 || io_threads_value > 128)
        {
            throw std::invalid_argument("port or io_threads is outside the supported range");
        }

        const DbConfig db_config = ReadDbConfig();
        const std::uint64_t max_outstanding_value = ParseNumber(
            EnvironmentValue("DB_MAX_OUTSTANDING_REQUESTS", "4096").c_str(),
            "DB_MAX_OUTSTANDING_REQUESTS");

        if (max_outstanding_value == 0 || max_outstanding_value > 1000000)
        {
            throw std::invalid_argument(
                "DB_MAX_OUTSTANDING_REQUESTS must be between 1 and 1000000");
        }

        const std::size_t max_outstanding_requests = static_cast<std::size_t>(max_outstanding_value);
        nebula::net::EventLoop loop;

        nebula::rpc::CoroutineService service(ordersystem::benchmark::DbBenchmarkService::descriptor());
        service.Register<Request, Response>(
            "Execute",
            [](nebula::rpc::RpcServerContext& context, const Request& request) -> nebula::coro::Task<Response>
            {
                Response response;

                LoopDatabaseContext* database = context.Loop()->GetContext<LoopDatabaseContext>();
                if (database == nullptr)
                {
                    response.set_success(false);
                    response.set_error("current IO Loop has no database context");
                    co_return response;
                }
                database->AssertInOwnerLoop();

                DbRequest db_request;
                try
                {
                    db_request = ConvertRequest(request);
                }
                catch (const std::exception& error)
                {
                    response.set_success(false);
                    response.set_error(error.what());
                    co_return response;
                }

                // await_suspend() 把请求交给当前 EventLoop 的 Pipeline 队列后立即挂起。
                // 对应结果和 Pipeline 同步点都收到后，同一个 EventLoop 再恢复这个 RPC 协程。
                DbResult result = co_await DbOperationAwaiter(*database, db_request);
                response.set_success(result.success);
                response.set_value(result.value);
                response.set_error(result.error);
                response.set_database_call_us(result.database_call_us);
                response.set_db_queue_wait_us(result.queue_wait_us);
                co_return response;
            });

        nebula::rpc::RpcServer server(&loop, "0.0.0.0", static_cast<std::uint16_t>(port_value));
        server.RegisterCoroutineService(&service);

        // 每个负责 RPC 连接的 IO Loop 启动前建立一条自己的 PostgreSQL 连接。
        // 连接建立和预处理语句初始化仍在启动阶段同步完成；请求阶段使用非阻塞 Pipeline。
        server.SetIoLoopInitCallback([db_config, max_outstanding_requests](nebula::net::EventLoop* io_loop)
            {
                io_loop->SetContext(std::make_unique<LoopDatabaseContext>(
                    io_loop, db_config, max_outstanding_requests));
            });
        server.Start(static_cast<std::size_t>(io_threads_value));

        std::cout << "OrderSystem PostgreSQL benchmark server started"
                  << " port=" << port_value
                  << " io_threads=" << io_threads_value
                  << " db_connections=" << io_threads_value
                  << " db_mode=event_loop_nonblocking"
                  << " db_pipeline_mode=enabled"
                  << " db_pipeline_sync=one_per_loop_turn"
                  << " db_max_outstanding_requests=" << max_outstanding_requests
                  << " db_host=" << EnvironmentValue("DB_HOST", "127.0.0.1")
                  << " db_port=" << db_config.port
                  << " db_name=" << EnvironmentValue("DB_NAME", "ordersystem_bench")
                  << std::endl;

        loop.Loop();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "order_db_benchmark_server: " << error.what() << '\n';
        return 1;
    }
}
