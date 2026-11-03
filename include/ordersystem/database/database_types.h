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

    std::uint64_t queue_wait_us{0};                                                 // 请求在数据库访问层排队的时间，单位：微秒（阶段 1 是等 DB Worker，阶段 2 是等所属 EventLoop 的连接）
    std::uint64_t database_call_us{0};                                              // SQL 真正开始发送到完整收完结果的时间，单位：微秒
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
