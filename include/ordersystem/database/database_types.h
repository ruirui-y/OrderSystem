#pragma once

#include <cstdint>
#include <string>

namespace ordersystem::benchmark
{

enum class DbOperation
{
    Read,
    Insert,
    Update,
    Delete
};

// key 对应记录 ID，value 只在 Insert 时使用。
struct DbRequest
{
    DbOperation operation{};
    std::uint64_t key{0};
    std::uint64_t value{0};
};

struct DbResult
{
    bool success{false};
    std::uint64_t value{0};
    std::string error;

    std::uint64_t queue_wait_us{0};                                                 // 请求从提交到写进 PostgreSQL 连接的时间，单位：微秒
    std::uint64_t database_call_us{0};                                              // 请求发出到收到同步点、确认结果的时间，单位：微秒
};

struct DbConfig
{
    std::string host{"127.0.0.1"};
    std::uint16_t port{5432};
    std::string user{"ordersystem_bench"};
    std::string password;
    std::string database{"ordersystem_bench"};
};

}  // namespace ordersystem::benchmark
