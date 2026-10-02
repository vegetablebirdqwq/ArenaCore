// tools/bot_client.cpp
// 演示 Bot：连接集成服务器，注册为玩家，自动发移动/技能指令，打印收到的快照。
// 行为模式（第 2 个参数，缺省=1 巡逻）：
//   1 巡逻 Wanderer（默认）：随机转向游走 + 偶发技能
//   2 冲锋 Aggressor：大步前冲 + 频繁技能
//   3 龟缩 Defender：小幅震荡 + 定期技能
// 这是压测 Bot 的前身（教程 M11）。
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "net/codec.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <direct.h>
#include <thread>

using namespace arena;

inline constexpr std::uint16_t kCmdMove = 0x0003;
inline constexpr std::uint16_t kCmdSkill = 0x0004;

// 收到的快照命令号（服务器广播）
inline constexpr std::uint16_t kCmdSnapshotFull = 0x0301;
inline constexpr std::uint16_t kCmdSnapshotDelta = 0x0302;

// 行为模式
enum BotMode : int {
    kModeWanderer = 1,   // 巡逻（默认）
    kModeAggressor = 2,  // 冲锋
    kModeDefender = 3,   // 龟缩
};

// 日志 tee：控制台 + logs/demo.log 双写（monitor.html 叠加显示服务器+bot）
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

// 轻量解析快照：读 frame + 实体数（前两个 varint）。返回实体数，失败返回 0。
static std::size_t parse_snapshot_count(const std::uint8_t* data, std::size_t size) {
    std::size_t off = 0;
    std::uint64_t frame = 0;
    if (!net::read_varint(data, size, off, frame)) return 0;
    std::uint64_t count = 0;
    if (!net::read_varint(data, size, off, count)) return 0;
    return static_cast<std::size_t>(count);
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

// 按模式决定本次动作：移动量 (dx,dy) + 是否放技能。
// tick 是 bot 自己的行为节拍（每 300ms 一次），决定行为不需要服务器信息。
struct BotAction {
    std::int16_t dx = 0;
    std::int16_t dy = 0;
    bool cast_skill = false;
};

static BotAction decide_action(int mode, int bot_id, std::uint64_t tick) {
    BotAction a;
    switch (mode) {
        case kModeAggressor: {   // 冲锋：大步前冲 + 频繁技能
            a.dx = 2;                                      // 每步 2 单位（服务端限幅上限内）
            a.dy = static_cast<std::int16_t>((tick / 5) % 3 - 1);
            a.cast_skill = (tick % 8 == 0);                // 每 8 tick 放一次
            break;
        }
        case kModeDefender: {    // 龟缩：小幅震荡 + 定期技能
            a.dx = static_cast<std::int16_t>((tick / 3) % 2 == 0 ? 0 : 1);
            a.dy = static_cast<std::int16_t>(((tick / 3) % 2) - 1);
            a.cast_skill = (tick % 10 == 0);
            break;
        }
        case kModeWanderer:
        default: {               // 巡逻（默认）：8 方向随机转向 + 偶发技能
            static const std::int8_t dirs[8][2] = {
                {1, 0}, {1, 1}, {0, 1}, {-1, 1},
                {-1, 0}, {-1, -1}, {0, -1}, {1, -1},
            };
            const int dir = static_cast<int>((tick / 5) % 8);   // 每 5 tick 换一次方向
            a.dx = dirs[dir][0];
            a.dy = dirs[dir][1];
            a.cast_skill = (tick % 12 == 0);
            break;
        }
    }
    // bot_id 奇偶让巡逻方向略有不同，观感更乱
    if (mode == kModeWanderer && (bot_id % 2 == 0)) {
        a.dy = -a.dy;
    }
    return a;
}

static const char* mode_name(int mode) {
    switch (mode) {
        case kModeAggressor: return "冲锋 Aggressor";
        case kModeDefender:  return "龟缩 Defender";
        default:             return "巡逻 Wanderer(默认)";
    }
}

int main(int argc, char** argv) {
    // 让控制台用 UTF-8 显示中文输出
    ::SetConsoleOutputCP(CP_UTF8);
    const int bot_id = (argc > 1) ? std::atoi(argv[1]) : 0;
    int mode = kModeWanderer;                       // 默认行为：巡逻
    if (argc > 2) {
        const int m = std::atoi(argv[2]);
        if (m >= 1 && m <= 3) mode = m;
    }
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
        // 服务器可能还没起，自动重试（最多 20 秒）
        log_line("[bot%d] connect 失败 err=%d，自动重试...\n", bot_id, ::WSAGetLastError());
        bool connected = false;
        for (int attempt = 0; attempt < 20; ++attempt) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            ::closesocket(s);
            s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s == INVALID_SOCKET) { break; }
            if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != SOCKET_ERROR) {
                connected = true;
                break;
            }
        }
        if (!connected) {
            log_line("[bot%d] 重试 20 秒仍连不上，退出\n", bot_id);
            return 1;
        }
    }
    log_line("[bot%d] 已连接，加入房间，行为模式=%s\n", bot_id, mode_name(mode));

    // 非阻塞：收快照 + 定时发指令
    u_long mode_nb = 1;
    ::ioctlsocket(s, FIONBIO, &mode_nb);

    std::uint16_t seq_move = 1;
    std::uint16_t seq_skill = 1;   // 技能用独立序号空间（教程 §7.2 幂等去重）
    std::uint64_t tick = 0;
    auto last_action = std::chrono::steady_clock::now();

    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (std::chrono::steady_clock::now() < end) {
        // ---- 收广播快照 ----
        std::uint8_t buf[4096];
        int n = ::recv(s, reinterpret_cast<char*>(buf), sizeof(buf), 0);
        if (n > 0) {
            std::size_t off = 0;
            while (off + 8 <= static_cast<std::size_t>(n)) {
                const std::uint32_t len = net::load_le32(buf + off);
                if (len + 4 > static_cast<std::size_t>(n) - off) break;  // 半包，等下次
                const std::uint16_t cmd = net::load_le16(buf + off + 4);
                if (cmd == kCmdSnapshotFull || cmd == kCmdSnapshotDelta) {
                    const std::size_t seen = parse_snapshot_count(buf + off + 8, len - 4);
                    log_line("[bot%d] 收到快照 cmd=%04X len=%u 看到%d个实体: ",
                             bot_id, cmd, len, static_cast<int>(seen));
                    print_hex(buf + off + 8, len - 4 > 24 ? 24 : len - 4);
                }
                off += 4 + len;
            }
        } else if (n == 0) {
            log_line("[bot%d] 服务器关闭连接\n", bot_id);
            break;
        }

        // ---- 每 300ms 按行为模式发一次指令 ----
        const auto now = std::chrono::steady_clock::now();
        if (now - last_action > std::chrono::milliseconds(300)) {
            last_action = now;
            ++tick;
            const BotAction act = decide_action(mode, bot_id, tick);

            // 移动指令（服务端限幅校验，超了会被拒/被夹）
            {
                std::uint8_t payload[4];
                net::store_le16(payload, static_cast<std::uint16_t>(act.dx));
                net::store_le16(payload + 2, static_cast<std::uint16_t>(act.dy));
                auto pkt = net::encode(kCmdMove, seq_move++, payload, 4);
                ::send(s, reinterpret_cast<const char*>(pkt.data()),
                       static_cast<int>(pkt.size()), 0);
            }
            // 技能指令（服务端校验射程/CD/队友，越界会拒——这正是服务端权威）
            if (act.cast_skill) {
                // 目标选对面一队的 bot（1002 或 1003，视自己队伍而定）
                const std::uint32_t target = (bot_id % 2 == 0) ? 1002u : 1000u;
                std::uint8_t sp[4];
                net::store_le16(sp, 1);   // skill_id=1
                net::store_le16(sp + 2, static_cast<std::uint16_t>(target & 0xFFFF));
                auto pkt = net::encode(kCmdSkill, seq_skill++, sp, 4);
                ::send(s, reinterpret_cast<const char*>(pkt.data()),
                       static_cast<int>(pkt.size()), 0);
                log_line("[bot%d] 尝试释放技能 → 目标 %u\n", bot_id, target);
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    ::closesocket(s);
    ::WSACleanup();
    log_line("[bot%d] 结束\n", bot_id);
    return 0;
}
