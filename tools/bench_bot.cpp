// tools/bench_bot.cpp
//
// ArenaCore 压测 Bot。一次跑完：建 N 条连接 → 循环发移动 → 收快照统计 RTT，
// 输出逐秒曲线 CSV + 汇总（P50/P90/P99/吞吐/错误）。
//
// 三个设计要点（面试必问）：
//  1. 复用服务器的 codec：同一个 net/codec.h + net/protocol_cmds.h。
//  2. N 条连接不能用 N 个线程：K 个线程，每个线程 WSAPoll 管 N/K 条非阻塞连接。
//  3. 非阻塞 connect：立刻返回 WSAEWOULDBLOCK，结果等 poll 报可写 + SO_ERROR。
//
// 压测目标：现有 game_server.exe（连上即进房，服务器广播快照）。
// RTT 测法：发一条移动 → 收到下一条快照 = 一次往返（服务器对客户端状态的回应）。
//
// 编译/运行（CMake 已配置，直接）：
//   bench_bot.exe --host 127.0.0.1 --port 9527 --conns 300 --threads 4 --duration 20 --out bench

#include "base/histogram.h"
#include "net/codec.h"
#include "net/protocol_cmds.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using arena::base::RingBuffer;
using RttHist = arena::base::LatencyHistogram<512, 500>;  // 500us 一档，量程 0~255.5ms

std::uint64_t now_us() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());
}

// ---------------------------------------------------------------- 参数
struct Options {
    std::string host = "127.0.0.1";
    unsigned short port = 9527;
    std::size_t conns = 300;
    std::size_t threads = 4;
    int duration_s = 20;
    int move_interval_ms = 100;  // 每个 Bot 每 100ms 发一次移动 = 10Hz
    int ramp_ms = 0;             // 线程之间的启动间隔，用来做"阶梯加压"
    std::string out_prefix = "bench";
};

void print_usage() {
    std::printf(
        "用法: bench_bot.exe [--host IP] [--port N] [--conns N] [--threads N]\n"
        "                   [--duration 秒] [--move-interval 毫秒] [--ramp 毫秒] [--out 文件名前缀]\n");
}

Options parse_args(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const char* v = (i + 1 < argc) ? argv[i + 1] : nullptr;
        auto need = [&]() -> std::string {
            if (v == nullptr) {
                std::printf("参数 %s 后面缺值\n", a.c_str());
                std::exit(2);
            }
            ++i;
            return std::string(v);
        };
        if (a == "--host") {
            o.host = need();
        } else if (a == "--port") {
            o.port = static_cast<unsigned short>(std::atoi(need().c_str()));
        } else if (a == "--conns") {
            o.conns = static_cast<std::size_t>(std::atoll(need().c_str()));
        } else if (a == "--threads") {
            o.threads = static_cast<std::size_t>(std::atoll(need().c_str()));
        } else if (a == "--duration") {
            o.duration_s = std::atoi(need().c_str());
        } else if (a == "--move-interval") {
            o.move_interval_ms = std::atoi(need().c_str());
        } else if (a == "--ramp") {
            o.ramp_ms = std::atoi(need().c_str());
        } else if (a == "--out") {
            o.out_prefix = need();
        } else if (a == "--help" || a == "-h") {
            print_usage();
            std::exit(0);
        } else {
            std::printf("不认识参数 %s\n", a.c_str());
            print_usage();
            std::exit(2);
        }
    }
    if (o.threads == 0) {
        o.threads = 1;
    }
    if (o.conns < o.threads) {
        o.threads = o.conns == 0 ? 1 : o.conns;
    }
    return o;
}

// ---------------------------------------------------------------- Bot
struct Bot {
    SOCKET sock = INVALID_SOCKET;
    int id = 0;
    bool connecting = true;    // 非阻塞 connect 还没确认
    bool connected = false;    // poll 报可写 + SO_ERROR==0

    RingBuffer in{2048};                       // 复用服务器的环形缓冲
    std::vector<std::uint8_t> out;             // 待发字节
    std::size_t out_pos = 0;

    std::uint16_t next_seq = 1;
    // 简化 RTT 测法：不跟踪每个 seq（服务器不回复 ack），
    // 而是"发 move 的时刻 → 收到下一条快照"算一次往返。
    std::uint64_t last_move_sent_us = 0;
    bool await_snapshot = false;

    std::uint64_t next_move_us = 0;
    std::uint64_t reconnect_at_us = 0;

    std::int32_t x = 0;
    std::int32_t y = 0;
    std::mt19937 rng{0};
};

struct ThreadStats {
    std::atomic<std::uint64_t> live{0};
    std::atomic<std::uint64_t> connected{0};
    std::atomic<std::uint64_t> sent{0};
    std::atomic<std::uint64_t> recv{0};
    std::atomic<std::uint64_t> bytes_in{0};
    std::atomic<std::uint64_t> bytes_out{0};
    std::atomic<std::uint64_t> errors{0};
    std::atomic<std::uint64_t> reconnects{0};
    std::atomic<std::uint64_t> snapshots{0};
    std::atomic<std::uint64_t> rtt_samples{0};
    RttHist rtt;                               // 只有本线程写，最后汇总才读
};

// ---------------------------------------------------------------- 收发
void mark_dead(Bot& b) {
    if (b.sock != INVALID_SOCKET) {
        ::closesocket(b.sock);
        b.sock = INVALID_SOCKET;
    }
    b.connecting = false;
    b.connected = false;
    b.in.consume(b.in.readable());
    b.out.clear();
    b.out_pos = 0;
    b.await_snapshot = false;
}

void flush(Bot& b, ThreadStats& st) {
    while (b.out_pos < b.out.size()) {
        const int n = ::send(b.sock,
                             reinterpret_cast<const char*>(b.out.data() + b.out_pos),
                             static_cast<int>(b.out.size() - b.out_pos),
                             0);
        if (n == SOCKET_ERROR) {
            const int err = ::WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                return;
            }
            st.errors.fetch_add(1, std::memory_order_relaxed);
            mark_dead(b);
            return;
        }
        b.out_pos += static_cast<std::size_t>(n);
        st.bytes_out.fetch_add(static_cast<std::size_t>(n), std::memory_order_relaxed);
    }
    b.out.clear();
    b.out_pos = 0;
}

/// 发一条移动指令，记下发出时刻（等收到快照算 RTT）。
void send_move(Bot& b, ThreadStats& st, std::int32_t dx, std::int32_t dy) {
    const std::uint16_t seq = b.next_seq++;
    std::vector<std::uint8_t> bytes = arena::net::encode(
        arena::net::kMoveReq, seq,
        arena::net::make_move_payload(dx, dy).data(),
        arena::net::make_move_payload(dx, dy).size());
    b.last_move_sent_us = now_us();
    b.await_snapshot = true;
    st.sent.fetch_add(1, std::memory_order_relaxed);
    b.out.insert(b.out.end(), bytes.begin(), bytes.end());
    flush(b, st);
}

void on_snapshot(Bot& b, ThreadStats& st) {
    st.snapshots.fetch_add(1, std::memory_order_relaxed);
    // 收到快照 = 服务器对我们状态的一次回应，正好用来测往返延迟
    if (b.await_snapshot && b.last_move_sent_us != 0) {
        const std::uint64_t rtt = now_us() - b.last_move_sent_us;
        st.rtt.record(rtt);
        st.rtt_samples.fetch_add(1, std::memory_order_relaxed);
        b.await_snapshot = false;
        b.last_move_sent_us = 0;
    }
}

void handle_packet(Bot& b, ThreadStats& st, const arena::net::Packet& pkt) {
    if (pkt.cmd == arena::net::kSnapshotFull || pkt.cmd == arena::net::kSnapshotDelta) {
        on_snapshot(b, st);
    }
}

bool on_readable(Bot& b, ThreadStats& st) {
    for (int round = 0; round < 64; ++round) {  // 单轮最多读 64 次，防止饿死别的连接
        b.in.ensure_writable(8192);
        const int n = ::recv(b.sock,
                             reinterpret_cast<char*>(b.in.write_ptr()),
                             static_cast<int>(b.in.writable()),
                             0);
        if (n == 0) {
            return false;
        }
        if (n == SOCKET_ERROR) {
            const int err = ::WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                break;
            }
            return false;
        }
        b.in.commit_write(static_cast<std::size_t>(n));
        st.bytes_in.fetch_add(static_cast<std::size_t>(n), std::memory_order_relaxed);
    }

    std::vector<arena::net::Packet> packets;
    const std::size_t got = arena::net::decode_all(b.in, packets, nullptr);
    if (got == arena::net::kDecodeError) {
        return false;
    }
    for (const arena::net::Packet& p : packets) {
        st.recv.fetch_add(1, std::memory_order_relaxed);
        handle_packet(b, st, p);
    }
    return true;
}

// ---------------------------------------------------------------- 建连
bool connect_async(Bot& b, const Options& opt, ThreadStats& st) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* res = nullptr;
    const std::string port_text = std::to_string(opt.port);
    if (::getaddrinfo(opt.host.c_str(), port_text.c_str(), &hints, &res) != 0) {
        return false;
    }
    SOCKET s = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET) {
        ::freeaddrinfo(res);
        return false;
    }
    u_long nonblocking = 1;
    ::ioctlsocket(s, FIONBIO, &nonblocking);
    BOOL nodelay = TRUE;
    ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

    const int rc = ::connect(s, res->ai_addr, static_cast<int>(res->ai_addrlen));
    ::freeaddrinfo(res);
    if (rc == SOCKET_ERROR) {
        const int err = ::WSAGetLastError();
        if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS) {
            ::closesocket(s);
            st.errors.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        // 正常路径：连接正在建立，等 poll 报可写
    }

    b.sock = s;
    b.connecting = true;
    b.connected = false;
    return true;
}

// ---------------------------------------------------------------- 线程主循环
void bot_thread(const Options& opt, int tid, std::size_t begin, std::size_t end,
                ThreadStats& st, std::atomic<bool>& stop) {
    if (opt.ramp_ms > 0 && tid > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long long>(opt.ramp_ms) * tid));
    }

    std::vector<std::unique_ptr<Bot>> bots;
    bots.reserve(end - begin);
    for (std::size_t i = begin; i < end; ++i) {
        auto b = std::make_unique<Bot>();
        b->id = static_cast<int>(i);
        b->rng.seed(0x9E3779B97F4A7C15ULL * (i + 1));
        if (connect_async(*b, opt, st)) {
            st.live.fetch_add(1, std::memory_order_relaxed);
            st.connected.fetch_add(1, std::memory_order_relaxed);
        }
        bots.push_back(std::move(b));
    }

    std::vector<WSAPOLLFD> fds;
    std::vector<Bot*> owners;
    fds.reserve(bots.size());
    owners.reserve(bots.size());

    while (!stop.load(std::memory_order_relaxed)) {
        const std::uint64_t now = now_us();

        fds.clear();
        owners.clear();
        std::uint64_t next_deadline = now + 200000;  // 没有待办也要每 200ms 醒一次

        for (auto& up : bots) {
            Bot& b = *up;
            if (b.sock == INVALID_SOCKET) {
                if (b.reconnect_at_us != 0 && now >= b.reconnect_at_us) {
                    if (connect_async(b, opt, st)) {
                        st.live.fetch_add(1, std::memory_order_relaxed);
                        st.connected.fetch_add(1, std::memory_order_relaxed);
                        st.reconnects.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        b.reconnect_at_us = now + 1000000;
                    }
                }
                continue;
            }
            WSAPOLLFD p{};
            p.fd = b.sock;
            p.events = POLLRDNORM;
            if (b.connecting || b.out_pos < b.out.size()) {
                p.events |= POLLWRNORM;
            }
            fds.push_back(p);
            owners.push_back(&b);

            if (b.next_move_us != 0 && b.next_move_us < next_deadline) {
                next_deadline = b.next_move_us;
            }
        }

        long timeout_ms = 200;
        if (next_deadline > now) {
            const std::uint64_t delta = (next_deadline - now) / 1000;
            timeout_ms = static_cast<long>(delta < 200 ? delta : 200);
        } else {
            timeout_ms = 0;
        }

        {
            const int rc = ::WSAPoll(fds.data(), static_cast<ULONG>(fds.size()), timeout_ms);
            if (rc > 0) {
                for (std::size_t i = 0; i < fds.size(); ++i) {
                    if (fds[i].revents == 0) {
                        continue;
                    }
                    Bot& b = *owners[i];
                    if (b.sock == INVALID_SOCKET) {
                        continue;
                    }
                    // 非阻塞 connect 完成判定：poll 报可写 + SO_ERROR
                    if (b.connecting && (fds[i].revents & POLLWRNORM) != 0) {
                        int so_error = 0;
                        int len = static_cast<int>(sizeof(so_error));
                        ::getsockopt(b.sock, SOL_SOCKET, SO_ERROR,
                                     reinterpret_cast<char*>(&so_error), &len);
                        if (so_error == 0) {
                            b.connecting = false;
                            b.connected = true;
                        } else {
                            st.errors.fetch_add(1, std::memory_order_relaxed);
                            st.live.fetch_sub(1, std::memory_order_relaxed);
                            mark_dead(b);
                            b.reconnect_at_us = now_us() + 2000000;
                        }
                    }
                    if ((fds[i].revents & POLLWRNORM) != 0 && b.out_pos < b.out.size()) {
                        flush(b, st);
                    }
                    if ((fds[i].revents & POLLRDNORM) != 0 && b.sock != INVALID_SOCKET) {
                        if (!on_readable(b, st)) {
                            st.live.fetch_sub(1, std::memory_order_relaxed);
                            mark_dead(b);
                            b.reconnect_at_us = now_us() + 2000000;
                        }
                    }
                }
            }
        }

        // 到期动作：进房后的周期移动
        const std::uint64_t now2 = now_us();
        for (auto& up : bots) {
            Bot& b = *up;
            if (b.sock == INVALID_SOCKET || !b.connected) {
                continue;
            }
            if (now2 >= b.next_move_us) {
                b.next_move_us = now2 + static_cast<std::uint64_t>(opt.move_interval_ms) * 1000;
                // 随机小步移动：让服务器真的算一次坐标与 AOI，而不是发重复包
                const std::int32_t dx = static_cast<std::int32_t>(b.rng() % 21) - 10;
                const std::int32_t dy = static_cast<std::int32_t>(b.rng() % 21) - 10;
                send_move(b, st, dx, dy);
            }
        }
    }

    for (auto& up : bots) {
        if (up->sock != INVALID_SOCKET) {
            ::closesocket(up->sock);
            up->sock = INVALID_SOCKET;
        }
    }
}

// ---------------------------------------------------------------- 采样与 CSV
struct Snapshot {
    std::uint64_t conns = 0, connected = 0;
    std::uint64_t sent = 0, recv = 0, bytes_in = 0, bytes_out = 0;
    std::uint64_t errors = 0, reconnects = 0, snapshots = 0;
};

Snapshot collect(const std::vector<std::unique_ptr<ThreadStats>>& stats) {
    Snapshot s;
    for (const auto& st : stats) {
        s.conns += st->live.load(std::memory_order_relaxed);
        s.connected += st->connected.load(std::memory_order_relaxed);
        s.sent += st->sent.load(std::memory_order_relaxed);
        s.recv += st->recv.load(std::memory_order_relaxed);
        s.bytes_in += st->bytes_in.load(std::memory_order_relaxed);
        s.bytes_out += st->bytes_out.load(std::memory_order_relaxed);
        s.errors += st->errors.load(std::memory_order_relaxed);
        s.reconnects += st->reconnects.load(std::memory_order_relaxed);
        s.snapshots += st->snapshots.load(std::memory_order_relaxed);
    }
    return s;
}

/// 把所有线程的直方图并进调用方给的 global 里。
/// LatencyHistogram 里有 std::atomic，不可拷贝不可移动，必须引用传入。
void merge_rtt(const std::vector<std::unique_ptr<ThreadStats>>& stats, RttHist& global) {
    for (const auto& st : stats) {
        global.merge_from(st->rtt);
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const Options opt = parse_args(argc, argv);

    WSADATA wsa{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::printf("WSAStartup 失败\n");
        return 1;
    }

    std::printf("压测: %s:%u, %zu 连接, %zu 线程, %d 秒, 每 bot %dms 一次移动\n",
                opt.host.c_str(), opt.port, opt.conns, opt.threads, opt.duration_s,
                opt.move_interval_ms);

    // 把连接平均分给每个线程
    std::vector<std::unique_ptr<ThreadStats>> stats(opt.threads);
    for (std::size_t t = 0; t < opt.threads; ++t) {
        stats[t] = std::make_unique<ThreadStats>();
    }
    const std::size_t per = opt.conns / opt.threads;
    std::size_t rem = opt.conns % opt.threads;

    std::atomic<bool> stop{false};
    std::vector<std::thread> pool;
    pool.reserve(opt.threads);
    std::size_t begin = 0;
    for (std::size_t t = 0; t < opt.threads; ++t) {
        std::size_t count = per + (t < rem ? 1 : 0);
        const std::size_t end = begin + count;
        pool.emplace_back(bot_thread, opt, static_cast<int>(t), begin, end,
                          std::ref(*stats[t]), std::ref(stop));
        begin = end;
    }

    // 每秒采样一次，写逐秒 CSV
    const std::string csv_path = opt.out_prefix + "_per_sec.csv";
    std::ofstream csv(csv_path);
    csv << "sec,conns,connected,sent,recv,errors,reconnects,snapshots,bytes_in,bytes_out\n";

    Snapshot prev{};
    const auto t0 = Clock::now();
    for (int s = 1; s <= opt.duration_s; ++s) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const Snapshot cur = collect(stats);
        const std::uint64_t d_sent = cur.sent - prev.sent;
        const std::uint64_t d_recv = cur.recv - prev.recv;
        const std::uint64_t d_errors = cur.errors - prev.errors;
        const std::uint64_t d_reconnects = cur.reconnects - prev.reconnects;
        const std::uint64_t d_snap = cur.snapshots - prev.snapshots;
        const std::uint64_t d_in = cur.bytes_in - prev.bytes_in;
        const std::uint64_t d_out = cur.bytes_out - prev.bytes_out;
        csv << s << ","
            << cur.conns << "," << cur.connected << ","
            << d_sent << "," << d_recv << ","
            << d_errors << "," << d_reconnects << ","
            << d_snap << "," << d_in << "," << d_out << "\n";
        prev = cur;
        std::printf("  %2ds: 连接=%llu 错误=%llu 快照=%llu/秒\n",
                    s,
                    static_cast<unsigned long long>(cur.conns),
                    static_cast<unsigned long long>(d_errors),
                    static_cast<unsigned long long>(d_snap));
    }
    csv.close();

    stop.store(true);
    for (std::thread& th : pool) {
        th.join();
    }

    // 汇总
    const Snapshot total = collect(stats);
    RttHist rtt_global;
    merge_rtt(stats, rtt_global);

    std::printf("\n===== 压测汇总 =====\n");
    std::printf("连接: %llu 存活 / %llu 累计建连, 错误 %llu, 重连 %llu\n",
                static_cast<unsigned long long>(total.conns),
                static_cast<unsigned long long>(total.connected),
                static_cast<unsigned long long>(total.errors),
                static_cast<unsigned long long>(total.reconnects));
    std::printf("收包 %llu (%.1f 包/秒), 快照 %llu\n",
                static_cast<unsigned long long>(total.recv),
                static_cast<double>(total.recv) / opt.duration_s,
                static_cast<unsigned long long>(total.snapshots));
    std::printf("RTT 样本 %llu: 平均 %.0fus  P50 %.0fus  P90 %.0fus  P99 %.0fus\n",
                static_cast<unsigned long long>(rtt_global.count()),
                rtt_global.average(),
                rtt_global.percentile(50.0),
                rtt_global.percentile(90.0),
                rtt_global.percentile(99.0));
    std::printf("逐秒数据: %s\n", csv_path.c_str());

    ::WSACleanup();
    return 0;
}
