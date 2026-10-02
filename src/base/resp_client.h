#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace arena::base {

/// Winsock 的初始化/清理。全局放一个就行，析构时自动 WSACleanup。
class WinsockGuard {
public:
    WinsockGuard();
    ~WinsockGuard();
    WinsockGuard(const WinsockGuard&) = delete;
    WinsockGuard& operator=(const WinsockGuard&) = delete;
    bool ok() const noexcept { return ok_; }

private:
    bool ok_ = false;
};

/// 一条 RESP 回复。RESP 一共就 5 种前缀，对应这里 5 种类型。
struct RedisReply {
    enum class Type { Nil, Status, Error, Integer, Bulk, Array };

    Type type = Type::Nil;
    std::string text;                  // Status(+)、Error(-)、Bulk($) 的内容
    long long integer = 0;             // Integer(:)
    std::vector<RedisReply> elements;  // Array(*)

    bool is_error() const noexcept { return type == Type::Error; }
    bool is_nil() const noexcept { return type == Type::Nil; }
    bool is_ok() const noexcept { return !is_error() && !is_nil(); }
    const std::string& str() const noexcept { return text; }

    /// 数组按 [成员1, 分数1, 成员2, 分数2, ...] 展平后取第 i 个，省得调用方写 .elements[i]。
    const std::string& at(std::size_t i) const;
    std::size_t size() const noexcept { return elements.size(); }
};

/// 一条**阻塞式** Redis 连接。
///
/// 它解决什么问题：项目要求零第三方依赖，所以 hiredis 不能用，
/// 得自己把 RESP 协议这一段讲到字节。好在 RESP 是文本协议里最简单的一类：
/// 一条命令就是一个"全部参数的字符串数组"，回复用前缀字符区分类型。
/// 阻塞 + 单线程用的模型在游戏服务器里完全够：Redis 是内网服务，
/// 一次命令往返几十微秒，一次 ZADD 的 RT 不会拖住逻辑帧。
///
/// 线程安全：**不是**线程安全的。一条连接同一时刻只能有一个线程用，
/// 多线程要用就配连接池（下面那个 RedisPool）。
class RedisConnection {
public:
    RedisConnection() = default;
    ~RedisConnection();
    RedisConnection(const RedisConnection&) = delete;
    RedisConnection& operator=(const RedisConnection&) = delete;

    /// 建立连接。timeout_ms 同时用作收发超时。
    bool connect_to(const std::string& host, std::uint16_t port, int timeout_ms, std::string* error);
    void close();
    bool healthy() const noexcept { return healthy_; }
    const std::string& last_error() const noexcept { return last_error_; }

    /// 执行一条命令。args[0] 是命令名（大小写随意）。socket 级故障会让连接变不健康。
    RedisReply command(const std::vector<std::string>& args);
    RedisReply command(std::initializer_list<std::string> args);

    // ---------------- 常用命令的薄封装，够这个项目用 ----------------
    RedisReply ping();
    RedisReply set(const std::string& key, const std::string& value);
    RedisReply setex(const std::string& key, int seconds, const std::string& value);
    RedisReply get(const std::string& key);
    RedisReply del(const std::string& key);
    RedisReply expire(const std::string& key, int seconds);
    RedisReply zadd(const std::string& key, double score, const std::string& member);
    RedisReply zincrby(const std::string& key, double delta, const std::string& member);
    RedisReply zrevrank(const std::string& key, const std::string& member);
    /// 排行榜：分数从高到低，取 [start, stop] 名（0 是第一名，-1 是最后一名）。
    RedisReply zrevrange_withscores(const std::string& key, long long start, long long stop);
    RedisReply zremrangebyrank(const std::string& key, long long start, long long stop);
    RedisReply hset(const std::string& key, const std::string& field, const std::string& value);
    RedisReply hgetall(const std::string& key);

private:
    bool send_all(const std::string& bytes);
    bool fill();                              // 从 socket 再读一段进 in_
    bool read_line(std::string& line);        // 读到 \r\n（结果不含 \r\n）
    bool read_exact(std::size_t n, std::string& out);
    bool parse_reply(RedisReply& out, int depth);
    bool fail(const std::string& what);

    std::uintptr_t sock_ = static_cast<std::uintptr_t>(-1);  // INVALID_SOCKET，但不想在头文件里引 winsock2.h
    std::uint16_t port_ = 0;
    int timeout_ms_ = 2000;
    bool healthy_ = false;
    std::string in_;        // 接收缓冲
    std::size_t pos_ = 0;   // 已消费到的位置
    std::string last_error_;
};

/// 固定容量的连接池。
///
/// 它解决什么问题：一条连接不能多线程共用，但"每来一个请求就 connect 一次"更贵
/// （三次握手 + 认证，毫秒级）。连接池让连接在进程里反复用，
/// 拿不到空闲连接时阻塞等待而不是无限建连接 —— 顺便起到了背压的作用。
class RedisPool {
public:
    RedisPool(std::string host, std::uint16_t port, std::size_t size, int timeout_ms = 2000);
    ~RedisPool();
    RedisPool(const RedisPool&) = delete;
    RedisPool& operator=(const RedisPool&) = delete;

    /// RAII 借还。析构自动还池；如果这条连接已经坏了，还池时会被丢掉并补一条新的。
    /// 用法：auto c = pool.acquire(); if (!c) { /* Redis 不可用，走降级分支 */ }
    class Lease {
    public:
        Lease() = default;
        ~Lease();
        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&& other) noexcept;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;

        RedisConnection* operator->() const noexcept { return conn_; }
        RedisConnection& operator*() const noexcept { return *conn_; }
        explicit operator bool() const noexcept { return conn_ != nullptr; }
        void release();

    private:
        friend class RedisPool;
        Lease(RedisPool* pool, RedisConnection* conn) noexcept : pool_(pool), conn_(conn) {}
        RedisPool* pool_ = nullptr;
        RedisConnection* conn_ = nullptr;
    };

    Lease acquire();
    std::size_t idle_count();
    std::size_t live_count();
    void close_all();

private:
    RedisConnection* take_idle_locked();
    void give_back(RedisConnection* conn);
    RedisConnection* make_connection();

    std::string host_;
    std::uint16_t port_;
    std::size_t capacity_;
    int timeout_ms_;

    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<std::unique_ptr<RedisConnection>> idle_;
    std::size_t live_ = 0;  // 已创建（含借出）的连接数，用来卡住容量上限
};

}  // namespace arena::base
