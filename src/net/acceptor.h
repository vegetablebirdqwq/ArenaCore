#pragma once

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>

namespace arena::net {

class IocpService;
class Session;

class Acceptor {
public:
    /// 工厂：给出一个已经 accept 好的 socket 和对端地址，返回一个建好的 Session。
    /// 返回 nullptr 表示不要这个连接（Acceptor 会替你 closesocket）。
    using SessionFactory = std::function<std::shared_ptr<Session>(SOCKET, const sockaddr_in&)>;

    Acceptor(IocpService& io, SessionFactory factory);
    ~Acceptor();

    Acceptor(const Acceptor&) = delete;
    Acceptor& operator=(const Acceptor&) = delete;

    /// ip 传 "0.0.0.0" 表示监听所有网卡。
    bool start(const char* ip, std::uint16_t port);
    void stop();

    std::uint16_t local_port() const noexcept { return local_port_; }

private:
    void accept_loop();

    IocpService& io_;
    SessionFactory factory_;
    SOCKET listen_sock_ = INVALID_SOCKET;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::uint16_t local_port_ = 0;
};

}  // namespace arena::net
