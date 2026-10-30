#include "ordersystem/db_pool.h"

#include "nebula/net/event_loop.h"
#include "nebula/rpc/rpc_coroutine.h"
#include "nebula/rpc/rpc_server.h"
#include "order_db_benchmark.pb.h"

#include <mysql.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{
using ordersystem::benchmark::DbConfig;
using ordersystem::benchmark::DbOperation;
using ordersystem::benchmark::DbOperationAwaiter;
using ordersystem::benchmark::DbPool;
using ordersystem::benchmark::DbRequest;
using ordersystem::benchmark::DbResult;
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

    const std::uint64_t port = ParseNumber(
        EnvironmentValue("DB_PORT", "3306").c_str(), "DB_PORT");
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

    if (result.key == 0)
    {
        throw std::invalid_argument("database key must be greater than zero");
    }
    return result;
}

}  // namespace

int main(int argc, char** argv)
{
    bool mysql_initialized = false;
    try
    {
        const std::uint64_t port_value = argc > 1 ? ParseNumber(argv[1], "port") : 9000;
        const std::uint64_t io_threads_value = argc > 2 ? ParseNumber(argv[2], "io_threads") : 4;
        const std::uint64_t db_workers_value = argc > 3 ? ParseNumber(argv[3], "db_workers") : 16;

        if (port_value == 0 || port_value > 65535 || io_threads_value == 0 ||
            io_threads_value > 128 || db_workers_value == 0 || db_workers_value > 512)
        {
            throw std::invalid_argument("port, io_threads or db_workers is outside the supported range");
        }

        if (mysql_library_init(0, nullptr, nullptr) != 0)
        {
            throw std::runtime_error("mysql_library_init failed");
        }
        mysql_initialized = true;
        {
            DbPool db_pool(ReadDbConfig(), static_cast<std::size_t>(db_workers_value));
            nebula::net::EventLoop loop;

            nebula::rpc::CoroutineService service(ordersystem::benchmark::DbBenchmarkService::descriptor());
            service.Register<Request, Response>(
                "Execute",
                [&db_pool](nebula::rpc::RpcServerContext& context,
                           const Request& request) -> nebula::coro::Task<Response>
                {
                    Response response;
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

                    DbResult result = co_await DbOperationAwaiter(
                        db_pool,
                        context.Loop(),
                        db_request);
                    response.set_success(result.success);
                    response.set_value(result.value);
                    response.set_error(result.error);
                    response.set_database_call_us(result.database_call_us);
                    response.set_db_queue_wait_us(result.queue_wait_us);
                    co_return response;
                });

            nebula::rpc::RpcServer server(
                &loop,
                "0.0.0.0",
                static_cast<std::uint16_t>(port_value));
            server.RegisterCoroutineService(&service);
            server.Start(static_cast<std::size_t>(io_threads_value));

            std::cout << "OrderSystem DB benchmark server started"
                      << " port=" << port_value
                      << " io_threads=" << io_threads_value
                      << " db_workers=" << db_pool.WorkerCount()
                      << " db_host=" << EnvironmentValue("DB_HOST", "127.0.0.1")
                      << " db_name=" << EnvironmentValue("DB_NAME", "ordersystem_bench")
                      << std::endl;

            loop.Loop();
        }
        mysql_library_end();
        mysql_initialized = false;
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "order_db_benchmark_server: " << error.what() << '\n';
        if (mysql_initialized)
        {
            mysql_library_end();
        }
        return 1;
    }
}
