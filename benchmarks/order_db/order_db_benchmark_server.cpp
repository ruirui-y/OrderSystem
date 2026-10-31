#include "ordersystem/database/db_pool.h"
#include "ordersystem/database/loop_database_context.h"

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

// 缩短本文件中使用的数据库类型和 Protobuf 类型名称。
using ordersystem::benchmark::DbConfig;
using ordersystem::benchmark::DbOperation;
using ordersystem::benchmark::DbOperationAwaiter;
using ordersystem::benchmark::DbPool;
using ordersystem::benchmark::DbRequest;
using ordersystem::benchmark::DbResult;
using ordersystem::benchmark::LoopDatabaseContext;
using Request = ordersystem::benchmark::DbBenchmarkRequest;
using Response = ordersystem::benchmark::DbBenchmarkResponse;

// 环境变量未设置时，使用传入的默认值。
std::string EnvironmentValue(const char* name, std::string fallback)
{
    const char* value = std::getenv(name);
    return value == nullptr ? std::move(fallback) : std::string(value);
}

// 转换数字文本，并检查是否存在无法转换的尾部字符。
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

// 从环境变量读取 PostgreSQL 连接配置，密码只从 DB_PASSWORD 获取。
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

// 转换 RPC 请求，并提前检查 PostgreSQL BIGINT 的取值范围。
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
    if (result.operation == DbOperation::Insert && result.value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    {
        throw std::invalid_argument("database value exceeds PostgreSQL BIGINT max");
    }
    return result;
}

}  // namespace

// 启动压测服务端，初始化数据库线程池并注册协程 RPC 服务。
int main(int argc, char** argv)
{
    try
    {
        // 参数依次为 RPC 端口、IO 线程数和数据库 Worker 数量；阶段 1 仍保留 DB Worker。
        const std::uint64_t port_value = argc > 1 ? ParseNumber(argv[1], "port") : 9000;
        const std::uint64_t io_threads_value = argc > 2 ? ParseNumber(argv[2], "io_threads") : 4;
        const std::uint64_t db_workers_value = argc > 3 ? ParseNumber(argv[3], "db_workers") : 16;

        if (port_value == 0 || port_value > 65535 || io_threads_value == 0 ||
            io_threads_value > 128 || db_workers_value == 0 || db_workers_value > 512)
        {
            throw std::invalid_argument("port, io_threads or db_workers is outside the supported range");
        }

        const DbConfig db_config = ReadDbConfig();

        // 先建立全部数据库连接，连接初始化失败时不启动 RPC 服务。
        DbPool db_pool(db_config, static_cast<std::size_t>(db_workers_value));
        nebula::net::EventLoop loop;

        // 数据库操作通过 co_await 挂起当前 RPC 协程，不阻塞处理网络事件的 EventLoop。
        nebula::rpc::CoroutineService service(ordersystem::benchmark::DbBenchmarkService::descriptor());
        service.Register<Request, Response>(
            "Execute",
            [&db_pool](nebula::rpc::RpcServerContext& context, const Request& request) -> nebula::coro::Task<Response>
            {
                Response response;

                // 阶段 1 中上下文只记录当前 IO Loop；实际数据库连接仍由 DbPool 的 Worker 持有。
                LoopDatabaseContext* loop_database = context.Loop()->GetContext<LoopDatabaseContext>();
                if (loop_database == nullptr)
                {
                    response.set_success(false);
                    response.set_error("current IO Loop has no database context");
                    co_return response;
                }
                loop_database->AssertInOwnerLoop();

                // 参数检查放在提交任务之前，避免无效请求占用数据库 Worker。
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

                // Worker 完成 SQL 后，将协程恢复任务投回原来的 IO Loop。
                DbResult result = co_await DbOperationAwaiter(db_pool, context.Loop(), db_request);
                response.set_success(result.success);
                response.set_value(result.value);
                response.set_error(result.error);
                response.set_database_call_us(result.database_call_us);
                response.set_db_queue_wait_us(result.queue_wait_us);
                co_return response;
            });

        nebula::rpc::RpcServer server(&loop, "0.0.0.0", static_cast<std::uint16_t>(port_value));
        server.RegisterCoroutineService(&service);

        // 每个 IO Loop 保存自己的上下文，阶段 1 不在这里创建数据库连接。
        server.SetIoLoopInitCallback([db_config](nebula::net::EventLoop* io_loop)
            {
                io_loop->SetContext(std::make_unique<LoopDatabaseContext>(io_loop, db_config));
            });
        server.Start(static_cast<std::size_t>(io_threads_value));

        // 输出本次运行配置，不打印数据库密码。
        std::cout << "OrderSystem PostgreSQL benchmark server started"
                  << " port=" << port_value
                  << " io_threads=" << io_threads_value
                  << " db_workers=" << db_pool.WorkerCount()
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