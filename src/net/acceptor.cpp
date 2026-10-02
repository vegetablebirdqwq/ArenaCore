#include "net/acceptor.h"

#include <cstdio>
#include <cstring>

#include "net/iocp_service.h"
#include "net/session.h"

namespace arena::net {

Acceptor::Acceptor(IocpService& io, SessionFactory factory)
    : io_(io), factory_(std::move(factory)) {}

Acceptor::~Acceptor() {
    stop();
}

bool Acceptor::start(const char* ip, std::uint16_t port) {
    if (running_.load()) {
        return false;
    }

    listen_sock_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock_ == INVALID_SOCKET) {
        std::printf("[acceptor] socket() 失败, err=%d\n", ::WSAGetLastError());
        return false;
    }

    const BOOL yes = TRUE;
    // Windows 上 SO_REUSEADDR 会把端口暴露给别人抢，正经服务端用 SO_EXCLUSIVEADDRUSE。
    ::setsockopt(listen_sock_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                 reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(port);
    if (::inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        std::printf("[acceptor] inet_pton 失败: %s\n", ip);
        ::closesocket(listen_sock_);
        listen_sock_ = INVALID_SOCKET;
        return false;
    }

    if (::bind(listen_sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        std::printf("[acceptor] bind 失败, err=%d\n", ::WSAGetLastError());
        ::closesocket(listen_sock_);
        listen_sock_ = INVALID_SOCKET;
        return false;
    }

    if (::listen(listen_sock_, SOMAXCONN) == SOCKET_ERROR) {
        std::printf("[acceptor] listen 失败, err=%d\n", ::WSAGetLastError());
        ::closesocket(listen_sock_);
        listen_sock_ = INVALID_SOCKET;
        return false;
    }

    local_port_ = port;
    running_.store(true);
    thread_ = std::thread([this] { accept_loop(); });
    std::printf("[acceptor] 监听 %s:%u 成功\n", ip, port);
    return true;
}

void Acceptor::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    // 在 Windows 上，从另一个线程 closesocket 会让正在阻塞的 accept 立刻返回 WSAENOTSOCK。
    // 这是文档化的行为，跟 Linux 不一样（Linux 上得靠 signal 或者 eventfd）。
    if (listen_sock_ != INVALID_SOCKET) {
        ::closesocket(listen_sock_);
        listen_sock_ = INVALID_SOCKET;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    std::printf("[acceptor] 已停止\n");
}

void Acceptor::accept_loop() {
    std::printf("[acceptor] accept 线程启动, tid=%lu\n", ::GetCurrentThreadId());
    while (running_.load(std::memory_order_acquire)) {
        sockaddr_in peer{};
        int peer_len = static_cast<int>(sizeof(peer));

        const SOCKET client = ::accept(listen_sock_, reinterpret_cast<sockaddr*>(&peer), &peer_len);
        if (client == INVALID_SOCKET) {
            const int err = ::WSAGetLastError();
            if (!running_.load(std::memory_order_acquire)) {
                break;   // 我们自己调的 stop()，正常退出
            }
            if (err == WSAENOTSOCK || err == WSAEINTR) {
                break;
            }
            std::printf("[acceptor] accept 失败, err=%d\n", err);
            continue;
        }

        // 关掉 Nagle：游戏服务器必做，否则会和延迟 ACK 撞出 40ms 的神秘延迟
        const int nodelay = 1;
        ::setsockopt(client, IPPROTO_TCP, TCP_NODELAY,
                     reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

        // 交给工厂去建 Session。Session 的构造函数里会把 socket 绑到完成端口。
        std::shared_ptr<Session> session = factory_(client, peer);
        if (!session) {
            ::closesocket(client);
        }
    }
    std::printf("[acceptor] accept 线程退出\n");
}

}  // namespace arena::net
