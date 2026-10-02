// tools/echo_server_smoke.cpp
// 端到端冒烟 v3：IocpService + Acceptor + Session 完整收发 + 心跳踢人。
// 验证链路：
//   收发：客户端发协议包 → decode_all 解包 → on_packet 回调 → echo 回包
//   心跳：每 10 秒发 Ping，30 秒没动静踢连接
// 用法：起服务器后，跑 echo_client 验证收发；连上挂 30 秒验证踢人。
#include "net/acceptor.h"
#include "net/iocp_service.h"
#include "net/session.h"
#include "net/session_manager.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

class EchoServer {
public:
    bool start() {
        if (::WSAStartup(MAKEWORD(2, 2), &wsa_) != 0) {
            std::printf("[server] WSAStartup 失败\n");
            return false;
        }

        io_.start();

        acceptor_ = std::make_unique<arena::net::Acceptor>(
            io_, [this](SOCKET sock, const sockaddr_in& peer) {
                return on_accept(sock, peer);
            });

        if (!acceptor_->start("127.0.0.1", 9527)) {
            std::printf("[server] acceptor 启动失败\n");
            return false;
        }

        running_.store(true);
        heartbeat_thread_ = std::thread([this] { heartbeat_loop(); });
        return true;
    }

    void run_for(std::chrono::seconds secs) {
        std::this_thread::sleep_for(secs);
    }

    void stop() {
        running_.store(false);
        if (heartbeat_thread_.joinable()) {
            heartbeat_thread_.join();
        }
        if (acceptor_) {
            acceptor_->stop();
        }
        io_.stop();
        ::WSACleanup();
    }

private:
    std::shared_ptr<arena::net::Session> on_accept(SOCKET sock, const sockaddr_in& peer) {
        char ip[32] = {};
        ::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        char peer_text[64] = {};
        std::snprintf(peer_text, sizeof(peer_text), "%s:%u", ip, ::ntohs(peer.sin_port));

        auto session = std::make_shared<arena::net::Session>(
            io_, sock, sessions_.next_id(), peer_text);

        session->set_packet_callback(
            [this](const std::shared_ptr<arena::net::Session>& s, arena::net::Packet&& pkt) {
                on_packet(s, std::move(pkt));
            });

        session->set_close_callback([this](const std::shared_ptr<arena::net::Session>& s) {
            std::printf("[server] 连接关闭: id=%llu %s\n",
                        static_cast<unsigned long long>(s->id()), s->peer_text().c_str());
            sessions_.remove(s->id());
        });

        session->start();
        sessions_.add(session);

        std::printf("[server] 新连接: id=%llu %s, 当前连接数=%zu\n",
                    static_cast<unsigned long long>(session->id()),
                    session->peer_text().c_str(), sessions_.size());
        return session;
    }

    void on_packet(const std::shared_ptr<arena::net::Session>& s, arena::net::Packet&& pkt) {
        if (pkt.cmd == arena::net::kCmdHeartbeatPong) {
            // Pong 不需要回，收包本身已经刷新了 last_active（touch 在 handle_recv 里）
            return;
        }
        std::printf("[server] id=%llu cmd=%u seq=%u payload=%zu 字节\n",
                    static_cast<unsigned long long>(s->id()), pkt.cmd, pkt.seq,
                    pkt.payload.size());
        // echo 回去
        s->send(arena::net::encode(pkt.cmd, pkt.seq, pkt.payload.data(), pkt.payload.size()));
    }

    void heartbeat_loop() {
        using namespace std::chrono;
        auto last_beat = steady_clock::now();

        while (running_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(milliseconds(500));

            const auto now = steady_clock::now();
            const bool beat_time = (now - last_beat) >= seconds(10);
            if (beat_time) {
                last_beat = now;
            }

            for (const auto& session : sessions_.snapshot()) {
                if (session->closed()) {
                    continue;
                }

                if (now - session->last_active() > seconds(30)) {
                    std::printf("[heartbeat] session %llu 已经 30 秒没动静了，踢\n",
                                static_cast<unsigned long long>(session->id()));
                    session->close();
                    continue;
                }

                if (beat_time) {
                    session->send(arena::net::encode(arena::net::kCmdHeartbeatPing, 0));
                }
            }
        }
    }

    WSADATA wsa_{};
    arena::net::IocpService io_{0};
    arena::net::SessionManager sessions_;
    std::unique_ptr<arena::net::Acceptor> acceptor_;
    std::thread heartbeat_thread_;
    std::atomic<bool> running_{false};
};

int main() {
    EchoServer server;
    if (!server.start()) {
        return 1;
    }
    std::printf("[main] echo 服务器就绪 127.0.0.1:9527（30 秒后自动退出）\n");
    std::printf("[main] 验证：echo_client 测收发；连上挂 30 秒测踢人\n");
    server.run_for(std::chrono::seconds(30));
    server.stop();
    std::printf("[main] 退出\n");
    return 0;
}
