// tools/game_server_smoke.cpp
// 集成验证 v3：函数式 main（与 echo_smoke 同构，避免类成员栈问题）。
//   预填 4 bot，真实客户端连上凑满 6 人 → Room 进 kFighting → 30Hz 结算 → 广播。
#include "net/acceptor.h"
#include "net/iocp_service.h"
#include "net/session.h"
#include "net/session_manager.h"
#include "game/room.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <direct.h>
#include <mutex>
#include <thread>

using namespace arena;

inline constexpr std::uint16_t kCmdMove = 0x0003;
inline constexpr std::uint16_t kCmdSkill = 0x0004;

// 日志 tee：控制台 + logs/demo.log 双写（monitor.html 用 iframe 加载显示）。
// 用「每次打开-追加-关闭」保证多进程写同一文件不互踩（O_APPEND 原子）。
// 用绝对路径：不管从哪个目录启动，日志都落在项目 logs\ 下。
static const char* kLogPath = "E:/projects/ArenaCore/logs/demo.log";
static void log_line(const char* fmt, ...) {
    std::va_list args1, args2;
    va_start(args1, fmt);
    va_copy(args2, args1);
    std::vfprintf(stdout, fmt, args1);
    std::fflush(stdout);
    va_end(args1);
    FILE* f = std::fopen(kLogPath, "a");
    if (f) {
        std::vfprintf(f, fmt, args2);
        std::fclose(f);
    }
    va_end(args2);
}

int main() {
    // 让控制台用 UTF-8 显示中文输出（否则 GBK 代码页下中文乱码）
    ::SetConsoleOutputCP(CP_UTF8);
    // 注意：MSVC 的 setvbuf 用 _IOLBF + nullptr buffer 会崩（0xC0000409）。
    // 要么提供 buffer，要么用 _IONBF。这里用 _IONBF（每 printf 立即写）。
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // 确保日志目录存在
    ::_mkdir("E:/projects/ArenaCore/logs");

    WSADATA wsa{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::fprintf(stderr, "[main] WSAStartup 失败\n");
        return 1;
    }

    net::IocpService io(0);
    net::SessionManager mgr;
    game::Room room;
    std::mutex room_mutex;
    std::atomic<bool> running{true};

    // 注入 Room 广播出口：player_id → Session
    // 每秒打印一次广播汇总（30Hz 每帧都打会刷屏，看不清）
    struct BcastStats {
        std::uint64_t count = 0;
        std::uint64_t bytes = 0;
        decltype(std::chrono::steady_clock::now()) last_report =
            std::chrono::steady_clock::now();
    };
    auto* bstats = new BcastStats();   // 生命周期 = 进程，无所谓泄漏
    room.set_send_fn([&, bstats](std::uint32_t player_id, std::uint16_t cmd,
                                 const std::vector<std::uint8_t>& payload) {
        bstats->count += 1;
        bstats->bytes += payload.size();
        for (const auto& s : mgr.snapshot()) {
            if (s->id() == player_id) {
                s->send(net::encode(cmd, 0, payload.data(), payload.size()));
                break;
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - bstats->last_report > std::chrono::seconds(1)) {
            log_line("[game] 广播汇总: 过去 1 秒 %llu 次 / %llu 字节\n",
                        static_cast<unsigned long long>(bstats->count),
                        static_cast<unsigned long long>(bstats->bytes));
            bstats->count = 0;
            bstats->bytes = 0;
            bstats->last_report = now;
        }
    });

    // 预填 4 个 bot
    room.add_player(1000, 0, "bot0");
    room.add_player(1001, 0, "bot1");
    room.add_player(1002, 1, "bot2");
    room.add_player(1003, 1, "bot3");
    log_line("[main] 预填 4 bot，room 状态=%d\n", static_cast<int>(room.state()));

    io.start();

    net::Acceptor acceptor(io, [&](SOCKET sock, const sockaddr_in& peer) {
        char ip[32] = {};
        ::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        char pt[64] = {};
        std::snprintf(pt, sizeof(pt), "%s:%u", ip, ::ntohs(peer.sin_port));

        auto session = std::make_shared<net::Session>(io, sock, mgr.next_id(), pt);
        const std::uint32_t player_id = static_cast<std::uint32_t>(session->id());
        const std::uint8_t team = (player_id % 2 == 0) ? 0u : 1u;

        session->set_packet_callback(
            [&, player_id](const std::shared_ptr<net::Session>&, net::Packet&& pkt) {
                std::lock_guard<std::mutex> lk(room_mutex);
                if (pkt.cmd == kCmdMove && pkt.payload.size() >= 4) {
                    game::MoveInput in;
                    in.player_id = player_id;
                    in.seq = pkt.seq;
                    in.dx = static_cast<std::int32_t>(static_cast<std::int16_t>(
                        static_cast<std::uint16_t>(pkt.payload[0]) |
                        (static_cast<std::uint16_t>(pkt.payload[1]) << 8)));
                    in.dy = static_cast<std::int32_t>(static_cast<std::int16_t>(
                        static_cast<std::uint16_t>(pkt.payload[2]) |
                        (static_cast<std::uint16_t>(pkt.payload[3]) << 8)));
                    room.submit_move(in);
                } else if (pkt.cmd == kCmdSkill && pkt.payload.size() >= 4) {
                    game::SkillInput in;
                    in.player_id = player_id;
                    in.seq = pkt.seq;
                    in.skill_id = static_cast<std::uint16_t>(pkt.payload[0]) |
                                  (static_cast<std::uint16_t>(pkt.payload[1]) << 8);
                    in.target_id = static_cast<std::uint32_t>(pkt.payload[2]) |
                                   (static_cast<std::uint32_t>(pkt.payload[3]) << 8);
                    room.submit_skill(in);
                }
            });
        session->set_close_callback([&](const std::shared_ptr<net::Session>& s) {
            mgr.remove(s->id());
            log_line("[game] 玩家 %llu 断开\n", static_cast<unsigned long long>(s->id()));
        });

        session->start();
        mgr.add(session);

        game::RoomState st = game::RoomState::kWaiting;
        {
            std::lock_guard<std::mutex> lk(room_mutex);
            char name[16] = {};
            std::snprintf(name, sizeof(name), "p%u", player_id);
            room.add_player(player_id, team, name);
            st = room.state();
        }
        log_line("[game] 玩家 %u 加入，房间 %zu 人，状态=%d\n",
                    player_id, room.player_count(), static_cast<int>(st));

        if (st == game::RoomState::kFighting) {
            room.on_reconnect(player_id);
        }
        return session;
    });

    if (!acceptor.start("127.0.0.1", 9527)) {
        std::fprintf(stderr, "[main] acceptor 启动失败\n");
        return 1;
    }

    // 逻辑线程
    std::thread logic([&] {
        using namespace std::chrono;
        while (running.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(milliseconds(1));
            std::lock_guard<std::mutex> lk(room_mutex);
            room.tick(steady_clock::now());
        }
    });

    log_line("[main] 集成服务器就绪 127.0.0.1:9527（120 秒后退出，Ctrl+C 可提前结束）\n");
    std::this_thread::sleep_for(std::chrono::seconds(120));

    running.store(false);
    if (logic.joinable()) logic.join();
    acceptor.stop();
    io.stop();
    ::WSACleanup();
    std::printf("[main] 退出\n");
    return 0;
}
