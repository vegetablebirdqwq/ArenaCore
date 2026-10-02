// tools/echo_server_smoke.cpp
// 端到端冒烟：IocpService + Acceptor + Session（最小实现）
// 验证链路：监听 → accept 连接 → 建 Session（绑完成端口）→ 登记到管理器 → 连接关闭时清理。
// 用法：起服务后，另开终端用 PowerShell 连：
//   Test-NetConnection 127.0.0.1 -Port 9527   （或 telnet 127.0.0.1 9527）
#include "net/acceptor.h"
#include "net/iocp_service.h"
#include "net/session.h"
#include "net/session_manager.h"

#include <chrono>
#include <cstdio>
#include <thread>

int main() {
    WSADATA wsa{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::printf("[main] WSAStartup 失败\n");
        return 1;
    }

    arena::net::IocpService io(0);
    io.start();

    arena::net::SessionManager mgr;

    // 工厂：accept 到连接后，建 Session 并登记
    arena::net::Acceptor acceptor(io, [&](SOCKET sock, const sockaddr_in& peer) {
        char ip[32] = {};
        ::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        char peer_text[64] = {};
        std::snprintf(peer_text, sizeof(peer_text), "%s:%u", ip, ::ntohs(peer.sin_port));

        auto session = std::make_shared<arena::net::Session>(
            io, sock, mgr.next_id(), peer_text);
        session->set_close_callback([&](const std::shared_ptr<arena::net::Session>& s) {
            std::printf("[main] 连接关闭: id=%llu %s\n",
                        static_cast<unsigned long long>(s->id()), s->peer_text().c_str());
            mgr.remove(s->id());
        });
        session->start();
        mgr.add(session);

        std::printf("[main] 新连接: id=%llu %s, 当前连接数=%zu\n",
                    static_cast<unsigned long long>(session->id()),
                    session->peer_text().c_str(), mgr.size());
        return session;
    });

    if (!acceptor.start("127.0.0.1", 9527)) {
        std::printf("[main] acceptor 启动失败\n");
        io.stop();
        ::WSACleanup();
        return 1;
    }

    std::printf("[main] 服务器就绪，监听 127.0.0.1:9527（10 秒后自动退出）\n");
    std::printf("[main] 另开终端测试：Test-NetConnection 127.0.0.1 -Port 9527\n");

    // 跑 10 秒让外部连接有机会进来
    std::this_thread::sleep_for(std::chrono::seconds(10));

    acceptor.stop();
    io.stop();
    ::WSACleanup();
    std::printf("[main] 退出\n");
    return 0;
}
