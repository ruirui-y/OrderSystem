#pragma once

#include "ordersystem/database/db_pool.h"

#include "nebula/net/event_loop.h"

#include <stdexcept>
#include <utility>

namespace ordersystem::benchmark
{

// 保存某一个 IO EventLoop 对应的数据库配置。
// 注意：阶段 1 中它还不持有 PostgreSQL 连接，真正的连接由 DbPool 中的 DB Worker 持有。
// 目前保留这个上下文，是为了让每个 IO Loop 都能取得自己的数据库配置；后续阶段再把连接迁移到这里。
class LoopDatabaseContext final
{
public:
    // 创建上下文时记录所属 EventLoop，并确认构造动作发生在该 Loop 的线程中。
    LoopDatabaseContext(nebula::net::EventLoop* loop, DbConfig config)
        : loop_(loop),
          config_(std::move(config))
    {
        if (loop_ == nullptr)
        {
            throw std::invalid_argument("LoopDatabaseContext requires an EventLoop");
        }

        // 这个对象按 Loop 归属使用，创建时就检查线程，避免以后在错误的线程里操作它。
        loop_->AssertInLoopThread();
    }

    // 返回这个数据库上下文所属的 EventLoop。
    [[nodiscard]] nebula::net::EventLoop* Loop() const noexcept
    {
        return loop_;
    }

    // 返回建立 PostgreSQL 连接时需要的参数。
    [[nodiscard]] const DbConfig& Config() const noexcept
    {
        return config_;
    }

    // 在真正使用上下文前检查当前线程是否仍然是所属 Loop 的线程。
    void AssertInOwnerLoop() const
    {
        loop_->AssertInLoopThread();
    }

private:
    nebula::net::EventLoop* loop_;  // 只记录所属 Loop，不负责释放 Loop；Loop 的生命周期由 RPC Server 管理
    DbConfig config_;               // 当前 Loop 使用的 PostgreSQL 连接参数副本
};

}  // namespace ordersystem::benchmark
