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
#include <cstdarg>
#include <cstdio>
#include <direct.h>
#include <thread>

using namespace arena;

inline constexpr std::uint16_t kCmdMove = 0x0003;
inline constexpr std::uint16_t kCmdSkill = 0x0004;

// 收到的快照命令号（服务器广播）
inline constexpr std::uint16_t kCmdSnapshotFull = 0x0301;
inline constexpr std::uint16_t kCmdSnapshotDelta = 0x0302;

// 日志 tee：控制台 + logs/demo.log 双写（monitor.html 叠加显示服务器+bot）
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

static void print_hex(const std::uint8_t* p, std::size_t n) {
    char tmp[160];
    std::size_t off = 0;
    for (std::size_t i = 0; i < n && i < 40; ++i) {
        off += std::snprintf(tmp + off, sizeof(tmp) - off, "%02X ", p[i]);
    }
    if (n > 40) off += std::snprintf(tmp + off, sizeof(tmp) - off, "...");
    off += std::snprintf(tmp + off, sizeof(tmp) - off, "\n");
    log_line("%s", tmp);
}

int main(int argc, char** argv) {
    // 让控制台用 UTF-8 显示中文输出（否则 GBK 代码页下中文乱码）
    ::SetConsoleOutputCP(CP_UTF8);
    const int bot_id = (argc > 1) ? std::atoi(argv[1]) : 0;
    // MSVC：stdout 重定向时是块缓冲，强杀会丢；用 _IONBF 每 printf 立即写
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    ::_mkdir("E:/projects/ArenaCore/logs");

    WSADATA wsa{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;

    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { log_line("[bot%d] socket 失败\n", bot_id); return 1; }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(9527);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        log_line("[bot%d] connect 失败 err=%d\n", bot_id, ::WSAGetLastError());
        return 1;
    }
    log_line("[bot%d] 已连接，加入房间\n", bot_id);

    // 非阻塞：收快照 + 定时发指令
    u_long mode = 1;
    ::ioctlsocket(s, FIONBIO, &mode);

    std::uint16_t seq = 1;
    auto last_move = std::chrono::steady_clock::now();

    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(120);
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
                    log_line("[bot%d] 收到快照 cmd=%04X len=%u: ", bot_id, cmd, len);
                    print_hex(buf + off + 8, len - 4 > 24 ? 24 : len - 4);
                }
                off += 4 + len;
            }
        } else if (n == 0) {
            log_line("[bot%d] 服务器关闭连接\n", bot_id);
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
    log_line("[bot%d] 结束\n", bot_id);
    return 0;
}
