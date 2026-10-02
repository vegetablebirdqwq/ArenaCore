// tools/bot_client.cpp
// 演示 Bot：连接集成服务器，注册为玩家，自动发移动/技能指令，打印收到的快照。
// 连 6 个 = 满 6 人开打（3v3），服务器 30Hz 结算并广播。
// 这是压测 Bot 的前身（教程 M11）。
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "net/codec.h"

#include <chrono>
#include <cstdio>
#include <thread>

using namespace arena;

inline constexpr std::uint16_t kCmdMove = 0x0003;
inline constexpr std::uint16_t kCmdSkill = 0x0004;

// 收到的快照命令号（服务器广播）
inline constexpr std::uint16_t kCmdSnapshotFull = 0x0301;
inline constexpr std::uint16_t kCmdSnapshotDelta = 0x0302;

static void print_hex(const std::uint8_t* p, std::size_t n) {
    for (std::size_t i = 0; i < n && i < 40; ++i) {
        std::printf("%02X ", p[i]);
    }
    if (n > 40) std::printf("...");
    std::printf("\n");
}

int main(int argc, char** argv) {
    const int bot_id = (argc > 1) ? std::atoi(argv[1]) : 0;
    // MSVC：stdout 重定向时是块缓冲，强杀会丢；用 _IONBF 每 printf 立即写
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    WSADATA wsa{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;

    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { std::printf("[bot%d] socket 失败\n", bot_id); return 1; }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(9527);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        std::printf("[bot%d] connect 失败 err=%d\n", bot_id, ::WSAGetLastError());
        return 1;
    }
    std::printf("[bot%d] 已连接，加入房间\n", bot_id);

    // 非阻塞：收快照 + 定时发指令
    u_long mode = 1;
    ::ioctlsocket(s, FIONBIO, &mode);

    std::uint16_t seq = 1;
    auto last_move = std::chrono::steady_clock::now();

    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < end) {
        // ---- 收广播快照 ----
        std::uint8_t buf[4096];
        int n = ::recv(s, reinterpret_cast<char*>(buf), sizeof(buf), 0);
        if (n > 0) {
            // 可能有多个包连在一起，逐个解析
            std::size_t off = 0;
            while (off + 8 <= static_cast<std::size_t>(n)) {
                const std::uint32_t len = net::load_le32(buf + off);
                if (len + 4 > static_cast<std::size_t>(n) - off) break;  // 半包，等下次
                const std::uint16_t cmd = net::load_le16(buf + off + 4);
                if (cmd == kCmdSnapshotFull || cmd == kCmdSnapshotDelta) {
                    std::printf("[bot%d] 收到快照 cmd=%04X len=%u: ", bot_id, cmd, len);
                    print_hex(buf + off + 8, len - 4 > 24 ? 24 : len - 4);
                }
                off += 4 + len;
            }
        } else if (n == 0) {
            std::printf("[bot%d] 服务器关闭连接\n", bot_id);
            break;
        }

        // ---- 每 300ms 发一次移动指令（朝随机方向走 1 单位）----
        const auto now = std::chrono::steady_clock::now();
        if (now - last_move > std::chrono::milliseconds(300)) {
            last_move = now;
            const std::int16_t dx = static_cast<std::int16_t>(1 * 2);  // 每帧 2 单位上限内
            const std::int16_t dy = static_cast<std::int16_t>((bot_id % 3) - 1);
            std::uint8_t payload[4];
            net::store_le16(payload, static_cast<std::uint16_t>(dx));
            net::store_le16(payload + 2, static_cast<std::uint16_t>(dy));
            auto pkt = net::encode(kCmdMove, seq++, payload, 4);
            ::send(s, reinterpret_cast<const char*>(pkt.data()), static_cast<int>(pkt.size()), 0);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    ::closesocket(s);
    ::WSACleanup();
    std::printf("[bot%d] 结束\n", bot_id);
    return 0;
}
