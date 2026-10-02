#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace arena::net {

class Session;

/// 连接表。它同时兼任「ID 生成器」和「谁拿着强引用」两件事。
class SessionManager {
public:
    std::uint64_t next_id() noexcept { return next_id_.fetch_add(1, std::memory_order_relaxed); }

    void add(std::shared_ptr<Session> session);
    void remove(std::uint64_t id);

    /// 抓一份当前快照。返回的是 shared_ptr 的拷贝，所以拿到快照之后
    /// 就算某个连接中途关了，你手上这份引用也不会变成野指针。
    std::vector<std::shared_ptr<Session>> snapshot() const;

    std::size_t size() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, std::shared_ptr<Session>> sessions_;
    std::atomic<std::uint64_t> next_id_{1};
};

}  // namespace arena::net
