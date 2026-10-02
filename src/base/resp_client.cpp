#include "base/resp_client.h"

// winsock2.h 必须排在最前面：它和 windows.h 有顺序依赖，
// 一旦先被别的头（哪怕间接）引入了 windows.h，就会出现 winsock.h/winsock2.h 重定义。
#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace arena::base {

namespace {

constexpr int kMaxReplyDepth = 32;
constexpr std::size_t kRecvChunk = 8192;

/// 把整数转成字符串。Redis 的分数是 double，用 %.17g 保证来回不失真。
std::string format_double(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    return std::string(buf);
}

}  // namespace

// ---------------------------------------------------------------- WinsockGuard
WinsockGuard::WinsockGuard() {
    WSADATA wsa{};
    ok_ = ::WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}

WinsockGuard::~WinsockGuard() {
    if (ok_) {
        ::WSACleanup();
    }
}

// ---------------------------------------------------------------- RedisReply
const std::string& RedisReply::at(std::size_t i) const {
    static const std::string kEmpty;
    return i < elements.size() ? elements[i].text : kEmpty;
}

// ---------------------------------------------------------------- RedisConnection
RedisConnection::~RedisConnection() {
    close();
}

bool RedisConnection::fail(const std::string& what) {
    last_error_ = what;
    healthy_ = false;
    close();
    return false;
}

bool RedisConnection::connect_to(const std::string& host,
                                 std::uint16_t port,
                                 int timeout_ms,
                                 std::string* error) {
    close();
    timeout_ms_ = timeout_ms > 0 ? timeout_ms : 2000;

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* res = nullptr;
    const std::string port_text = std::to_string(port);
    if (::getaddrinfo(host.c_str(), port_text.c_str(), &hints, &res) != 0) {
        last_error_ = "getaddrinfo failed for " + host;
        if (error != nullptr) {
            *error = last_error_;
        }
        return false;
    }

    SOCKET s = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET) {
        ::freeaddrinfo(res);
        last_error_ = "socket() failed";
        if (error != nullptr) {
            *error = last_error_;
        }
        return false;
    }

    // Windows 的坑：SO_RCVTIMEO / SO_SNDTIMEO 要的是**毫秒 DWORD**，不是 Linux 那种 timeval。
    // 写混了不会报错，只会得到一个荒唐的超时值（比如把你的 2 秒当成 0.000002 秒）。
    const DWORD timeout = static_cast<DWORD>(timeout_ms_);
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    // 关掉 Nagle：Redis 命令都是小包，攒包只会白加 40ms 延迟。
    BOOL nodelay = TRUE;
    ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

    // 注意：阻塞 connect 不受上面 SO_SNDTIMEO 的约束（那是收发超时）。
    // 连不存在的端口会立刻收到 RST 返回 WSAECONNREFUSED；
    // 连"主机存在但端口被防火墙丢掉"的地址会卡几十秒 —— 那种情况只能用
    // 非阻塞 connect + select/poll（压测 Bot 那一节就是这么写的）。
    if (::connect(s, res->ai_addr, static_cast<int>(res->ai_addrlen)) == SOCKET_ERROR) {
        const int err = ::WSAGetLastError();
        ::closesocket(s);
        ::freeaddrinfo(res);
        last_error_ = "connect() failed, WSA error " + std::to_string(err);
        if (error != nullptr) {
            *error = last_error_;
        }
        return false;
    }
    ::freeaddrinfo(res);

    sock_ = static_cast<std::uintptr_t>(s);
    port_ = port;
    healthy_ = true;
    in_.clear();
    pos_ = 0;
    return true;
}

void RedisConnection::close() {
    if (sock_ != static_cast<std::uintptr_t>(-1)) {
        ::closesocket(static_cast<SOCKET>(sock_));
        sock_ = static_cast<std::uintptr_t>(-1);
    }
    healthy_ = false;
    in_.clear();
    pos_ = 0;
}

bool RedisConnection::send_all(const std::string& bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const int n = ::send(static_cast<SOCKET>(sock_),
                             bytes.data() + sent,
                             static_cast<int>(bytes.size() - sent),
                             0);
        if (n == SOCKET_ERROR) {
            return fail("send failed, WSA error " + std::to_string(::WSAGetLastError()));
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

bool RedisConnection::fill() {
    if (pos_ == in_.size()) {
        in_.clear();
        pos_ = 0;
    } else if (pos_ >= kRecvChunk) {
        in_.erase(0, pos_);  // 消费过的部分挪掉，避免缓冲无限增长
        pos_ = 0;
    }
    char chunk[kRecvChunk];
    const int n = ::recv(static_cast<SOCKET>(sock_), chunk, static_cast<int>(sizeof(chunk)), 0);
    if (n == 0) {
        return fail("peer closed the connection");
    }
    if (n == SOCKET_ERROR) {
        const int err = ::WSAGetLastError();
        if (err == WSAETIMEDOUT) {
            return fail("recv timed out after " + std::to_string(timeout_ms_) + " ms");
        }
        return fail("recv failed, WSA error " + std::to_string(err));
    }
    in_.append(chunk, static_cast<std::size_t>(n));
    return true;
}

bool RedisConnection::read_line(std::string& line) {
    for (;;) {
        const std::size_t at = in_.find("\r\n", pos_);
        if (at != std::string::npos) {
            line.assign(in_, pos_, at - pos_);
            pos_ = at + 2;
            return true;
        }
        if (!fill()) {
            return false;
        }
    }
}

bool RedisConnection::read_exact(std::size_t n, std::string& out) {
    while (in_.size() - pos_ < n) {
        if (!fill()) {
            return false;
        }
    }
    out.assign(in_, pos_, n);
    pos_ += n;
    return true;
}

/// RESP 解析。全部规则就这几行：
///   +xxx\r\n        简单状态
///   -xxx\r\n        错误
///   :123\r\n        整数
///   $5\r\nhello\r\n 批量字符串（$ 后面是字节数；$-1 表示 nil）
///   *2\r\n...       数组（后面跟 2 个回复；*-1 表示 nil 数组）
bool RedisConnection::parse_reply(RedisReply& out, int depth) {
    if (depth > kMaxReplyDepth) {
        return fail("reply nesting too deep");
    }
    std::string line;
    if (!read_line(line)) {
        return false;
    }
    if (line.empty()) {
        return fail("empty reply line");
    }

    const char kind = line[0];
    const std::string rest = line.substr(1);
    switch (kind) {
        case '+':
            out.type = RedisReply::Type::Status;
            out.text = rest;
            return true;
        case '-':
            out.type = RedisReply::Type::Error;
            out.text = rest;
            return true;
        case ':':
            out.type = RedisReply::Type::Integer;
            out.integer = std::strtoll(rest.c_str(), nullptr, 10);
            out.text = rest;
            return true;
        case '$': {
            const long long len = std::strtoll(rest.c_str(), nullptr, 10);
            if (len < 0) {
                out.type = RedisReply::Type::Nil;
                return true;
            }
            std::string body;
            if (!read_exact(static_cast<std::size_t>(len), body)) {
                return false;
            }
            std::string tail;
            if (!read_exact(2, tail)) {  // 吃掉结尾的 \r\n
                return false;
            }
            out.type = RedisReply::Type::Bulk;
            out.text = std::move(body);
            return true;
        }
        case '*': {
            const long long n = std::strtoll(rest.c_str(), nullptr, 10);
            if (n < 0) {
                out.type = RedisReply::Type::Nil;
                return true;
            }
            out.type = RedisReply::Type::Array;
            out.elements.clear();
            out.elements.resize(static_cast<std::size_t>(n));
            for (long long i = 0; i < n; ++i) {
                if (!parse_reply(out.elements[static_cast<std::size_t>(i)], depth + 1)) {
                    return false;
                }
            }
            return true;
        }
        default:
            return fail(std::string("unknown RESP type prefix: ") + kind);
    }
}

RedisReply RedisConnection::command(const std::vector<std::string>& args) {
    RedisReply reply;
    if (args.empty()) {
        reply.type = RedisReply::Type::Error;
        reply.text = "empty command";
        return reply;
    }
    if (!healthy_) {
        reply.type = RedisReply::Type::Error;
        reply.text = "connection is not healthy: " + last_error_;
        return reply;
    }

    // 组包：*N\r\n 然后每个参数写 $len\r\narg\r\n。
    // 一次 send 发出去，既省系统调用，也天然支持流水线（pipeline）。
    std::string out;
    out.reserve(64);
    out += '*';
    out += std::to_string(args.size());
    out += "\r\n";
    for (const std::string& a : args) {
        out += '$';
        out += std::to_string(a.size());
        out += "\r\n";
        out += a;
        out += "\r\n";
    }

    if (!send_all(out)) {
        reply.type = RedisReply::Type::Error;
        reply.text = last_error_;
        return reply;
    }
    if (!parse_reply(reply, 0)) {
        reply.type = RedisReply::Type::Error;
        reply.text = last_error_;
    }
    return reply;
}

RedisReply RedisConnection::command(std::initializer_list<std::string> args) {
    return command(std::vector<std::string>(args));
}

RedisReply RedisConnection::ping() {
    return command({"PING"});
}

RedisReply RedisConnection::set(const std::string& key, const std::string& value) {
    return command({"SET", key, value});
}

RedisReply RedisConnection::setex(const std::string& key, int seconds, const std::string& value) {
    return command({"SETEX", key, std::to_string(seconds), value});
}

RedisReply RedisConnection::get(const std::string& key) {
    return command({"GET", key});
}

RedisReply RedisConnection::del(const std::string& key) {
    return command({"DEL", key});
}

RedisReply RedisConnection::expire(const std::string& key, int seconds) {
    return command({"EXPIRE", key, std::to_string(seconds)});
}

RedisReply RedisConnection::zadd(const std::string& key, double score, const std::string& member) {
    return command({"ZADD", key, format_double(score), member});
}

RedisReply RedisConnection::zincrby(const std::string& key, double delta, const std::string& member) {
    return command({"ZINCRBY", key, format_double(delta), member});
}

RedisReply RedisConnection::zrevrank(const std::string& key, const std::string& member) {
    return command({"ZREVRANK", key, member});
}

RedisReply RedisConnection::zrevrange_withscores(const std::string& key, long long start, long long stop) {
    return command({"ZREVRANGE", key, std::to_string(start), std::to_string(stop), "WITHSCORES"});
}

RedisReply RedisConnection::zremrangebyrank(const std::string& key, long long start, long long stop) {
    return command({"ZREMRANGEBYRANK", key, std::to_string(start), std::to_string(stop)});
}

RedisReply RedisConnection::hset(const std::string& key, const std::string& field, const std::string& value) {
    return command({"HSET", key, field, value});
}

RedisReply RedisConnection::hgetall(const std::string& key) {
    return command({"HGETALL", key});
}

// ---------------------------------------------------------------- RedisPool
RedisPool::RedisPool(std::string host, std::uint16_t port, std::size_t size, int timeout_ms)
    : host_(std::move(host)), port_(port), capacity_(size == 0 ? 1 : size), timeout_ms_(timeout_ms) {}

RedisPool::~RedisPool() {
    close_all();
}

RedisConnection* RedisPool::make_connection() {
    auto conn = std::make_unique<RedisConnection>();
    if (!conn->connect_to(host_, port_, timeout_ms_, nullptr)) {
        return nullptr;
    }
    return conn.release();
}

RedisConnection* RedisPool::take_idle_locked() {
    if (idle_.empty()) {
        return nullptr;
    }
    RedisConnection* conn = idle_.back().release();
    idle_.pop_back();
    return conn;
}

RedisPool::Lease RedisPool::acquire() {
    std::unique_lock<std::mutex> lock(mu_);
    for (;;) {
        if (RedisConnection* conn = take_idle_locked()) {
            return Lease(this, conn);
        }
        if (live_ < capacity_) {
            ++live_;  // 先占坑，再放锁去建连接，避免两个线程同时建出多余连接
            lock.unlock();
            RedisConnection* conn = make_connection();
            if (conn == nullptr) {
                lock.lock();
                --live_;
                cv_.notify_one();
                return Lease();  // 借不到：调用方走降级分支
            }
            return Lease(this, conn);
        }
        // 池子满且都在借出状态：等着，这就是背压。
        cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms_));
        if (live_ == 0 && idle_.empty()) {
            return Lease();
        }
    }
}

void RedisPool::give_back(RedisConnection* conn) {
    std::unique_lock<std::mutex> lock(mu_);
    if (conn == nullptr) {
        // Lease 是空的，什么都不用做
    } else if (conn->healthy()) {
        idle_.emplace_back(conn);
    } else {
        delete conn;
        --live_;  // 坏连接丢掉，下次 acquire 会补一条新的
    }
    cv_.notify_one();
}

RedisPool::Lease::~Lease() {
    release();
}

RedisPool::Lease::Lease(Lease&& other) noexcept : pool_(other.pool_), conn_(other.conn_) {
    other.pool_ = nullptr;
    other.conn_ = nullptr;
}

RedisPool::Lease& RedisPool::Lease::operator=(Lease&& other) noexcept {
    if (this != &other) {
        release();
        pool_ = other.pool_;
        conn_ = other.conn_;
        other.pool_ = nullptr;
        other.conn_ = nullptr;
    }
    return *this;
}

void RedisPool::Lease::release() {
    if (pool_ != nullptr) {
        // give_back 会把裸指针重新接管到 unique_ptr 里
        RedisConnection* conn = conn_;
        RedisPool* pool = pool_;
        pool_ = nullptr;
        conn_ = nullptr;
        pool->give_back(conn);
    }
}

std::size_t RedisPool::idle_count() {
    std::lock_guard<std::mutex> lock(mu_);
    return idle_.size();
}

std::size_t RedisPool::live_count() {
    std::lock_guard<std::mutex> lock(mu_);
    return live_;
}

void RedisPool::close_all() {
    std::lock_guard<std::mutex> lock(mu_);
    idle_.clear();
    live_ = 0;
}

}  // namespace arena::base
