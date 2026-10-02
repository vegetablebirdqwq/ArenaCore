// tools/echo_server_smoke.cpp
// 端到端冒烟 v2：IocpService + Acceptor + Session 完整收发。
// 验证链路：
//   监听 → accept → 绑端口 → 登记
//   → 客户端发协议包（len|cmd|seq|payload）
//   → WSARecv 完成 → decode_all 解包 → on_packet 回调打印
//   → 服务器 echo 回包 → 客户端收回来
// 用法：起服务器后，另开终端跑 tools/client_smoke.cpp（见 CMake echo_client 目标）。
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

    arena::net::Acceptor acceptor(io, [&](SOCKET sock, const sockaddr_in& peer) {
        char ip[32] = {};
        ::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        char peer_text[64] = {};
        std::snprintf(peer_text, sizeof(peer_text), "%s:%u", ip, ::ntohs(peer.sin_port));

        auto session = std::make_shared<arena::net::Session>(
            io, sock, mgr.next_id(), peer_text);

        // 收包回调：打印收到的包，然后 echo 回去
        session->set_packet_callback(
            [&](const std::shared_ptr<arena::net::Session>& s, arena::net::Packet&& pkt) {
                std::printf("[echo] id=%llu cmd=%u seq=%u payload=%zu 字节: ",
                            static_cast<unsigned long long>(s->id()), pkt.cmd, pkt.seq,
                            pkt.payload.size());
                for (std::uint8_t b : pkt.payload) {
                    std::printf("%02X ", b);
                }
                std::printf("\n");
                // echo：把同一个包发回去
                s->send(arena::net::encode(pkt.cmd, pkt.seq, pkt.payload.data(), pkt.payload.size()));
            });

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

    std::printf("[main] echo 服务器就绪 127.0.0.1:9527（15 秒后自动退出）\n");
    std::this_thread::sleep_for(std::chrono::seconds(15));

    acceptor.stop();
    io.stop();
    ::WSACleanup();
    std::printf("[main] 退出\n");
    return 0;
}
