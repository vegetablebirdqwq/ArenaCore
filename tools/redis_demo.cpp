// tools/redis_demo.cpp
//
// 演示三件事：
//   1. 自己写的 RESP 客户端能连通、能跑 PING/SET/GET
//   2. 排行榜（zset：ZADD / ZINCRBY / ZREVRANGE WITHSCORES / ZREVRANK）
//   3. 会话存储（SETEX / GET / TTL / HSET / HGETALL）
// 顺便量一下"每条命令一次往返"的成本，作为后面上连接池和批量写的依据。
//
// 启动 Redis（本机 D:\redis）：
//   D:\redis\redis-server.exe --port 6399
// 运行：
//   redis_demo.exe 127.0.0.1 6399

#include "base/resp_client.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using arena::base::RedisConnection;
using arena::base::RedisPool;
using arena::base::WinsockGuard;

namespace {

constexpr const char* kRankKey = "arena:rank:weekly";
constexpr const char* kRankKeyPrev = "arena:rank:weekly:rolling";

void print_reply(const char* tag, const arena::base::RedisReply& r) {
    if (r.is_error()) {
        std::printf("%-28s -> ERROR: %s\n", tag, r.text.c_str());
        return;
    }
    switch (r.type) {
        case arena::base::RedisReply::Type::Status:
            std::printf("%-28s -> +%s\n", tag, r.text.c_str());
            break;
        case arena::base::RedisReply::Type::Integer:
            std::printf("%-28s -> (integer) %lld\n", tag, r.integer);
            break;
        case arena::base::RedisReply::Type::Bulk:
            std::printf("%-28s -> \"%s\"\n", tag, r.text.c_str());
            break;
        case arena::base::RedisReply::Type::Nil:
            std::printf("%-28s -> (nil)\n", tag);
            break;
        case arena::base::RedisReply::Type::Array:
            std::printf("%-28s -> [%zu 项] ", tag, r.elements.size());
            for (const auto& e : r.elements) {
                std::printf("%s ", e.text.c_str());
            }
            std::printf("\n");
            break;
    }
}

void demo_basic(RedisConnection& c) {
    std::printf("---- 1. 基本连通性 ----\n");
    print_reply("PING", c.ping());
    print_reply("SET arena:hello", c.set("arena:hello", "world"));
    print_reply("GET arena:hello", c.get("arena:hello"));
    print_reply("GET 不存在的 key", c.get("arena:does-not-exist"));
    // 故意打一条错命令，验证 RESP 的 -ERR 分支能被正确解析
    print_reply("故意打错命令", c.command({"NOTACOMMAND", "x"}));
}

void demo_rank(RedisConnection& c) {
    std::printf("\n---- 2. 排行榜（zset）----\n");
    c.del(kRankKey);
    c.del(kRankKeyPrev);

    struct Row {
        const char* uid;
        double score;
    };
    const Row rows[] = {
        {"u1001", 1580}, {"u1002", 2310}, {"u1003", 1990},
        {"u1004", 2750}, {"u1005", 1210}, {"u1006", 2050},
    };
    for (const Row& r : rows) {
        char label[64];
        std::snprintf(label, sizeof(label), "ZADD %s %s", kRankKey, r.uid);
        print_reply(label, c.zadd(kRankKey, r.score, r.uid));
    }
    print_reply("ZINCRBY u1005 +500", c.zincrby(kRankKey, 500, "u1005"));
    print_reply("ZREVRANGE 0 -1 WITHSCORES", c.zrevrange_withscores(kRankKey, 0, -1));
    print_reply("ZREVRANGE 0 2（前 3 名）", c.zrevrange_withscores(kRankKey, 0, 2));
    print_reply("ZREVRANK u1005（0 起）", c.zrevrank(kRankKey, "u1005"));

    // 滚动榜：把老榜 0 分以上的分数累加到上一周期榜里，再删掉超出 100 名的尾部。
    print_reply("ZREMRANGEBYRANK 老榜 100 -1", c.zremrangebyrank(kRankKey, 100, -1));
}

void demo_session(RedisConnection& c) {
    std::printf("\n---- 3. 会话存储 ----\n");
    const std::string token = "tok:9f2c1a";
    print_reply("SETEX token 60", c.setex(token, 60, "uid=1001;room=42"));
    print_reply("GET token", c.get(token));
    print_reply("TTL token", c.command({"TTL", token}));
    print_reply("HSET room:42", c.hset("room:42", "map", "arena_01"));
    print_reply("HSET room:42", c.hset("room:42", "players", "6"));
    print_reply("HGETALL room:42", c.hgetall("room:42"));
}

/// 量一下单条命令的往返成本：这决定了"写合并"到底值不值得做。
void bench_roundtrip(RedisConnection& c, int n) {
    std::printf("\n---- 4. 单条命令往返成本（%d 次 SET）----\n", n);
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i) {
        c.set("arena:bench:" + std::to_string(i), "v");
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double sec = std::chrono::duration<double>(t1 - t0).count();
    std::printf("一次一条: %.0f 条/秒，单条往返 %.1f us\n",
                static_cast<double>(n) / sec, sec * 1e6 / static_cast<double>(n));
    c.del("arena:bench:0");
}

}  // namespace

int main(int argc, char** argv) {
    const std::string host = argc > 1 ? argv[1] : "127.0.0.1";
    const unsigned short port = static_cast<unsigned short>(argc > 2 ? std::atoi(argv[2]) : 6399);

    WinsockGuard winsock;
    if (!winsock.ok()) {
        std::printf("WSAStartup 失败，Redis 客户端用不了\n");
        return 1;
    }

    RedisConnection conn;
    std::string error;
    if (!conn.connect_to(host, port, 2000, &error)) {
        std::printf("连接 Redis %s:%u 失败：%s\n", host.c_str(), port, error.c_str());
        std::printf("先把服务起起来：D:\\redis\\redis-server.exe --port %u\n", port);
        return 1;
    }
    std::printf("已连接 Redis %s:%u\n", host.c_str(), port);

    demo_basic(conn);
    demo_rank(conn);
    demo_session(conn);
    bench_roundtrip(conn, 2000);

    // ---- 5. 连接池：4 条连接，并发跑 ----
    std::printf("\n---- 5. 连接池 ----\n");
    RedisPool pool(host, port, 4, 2000);
    {
        auto lease = pool.acquire();
        if (!lease) {
            std::printf("从池里借连接失败\n");
        } else {
            print_reply("池里借来的 PING", lease->ping());
            std::printf("空闲连接数=%zu, 已建连接数=%zu\n", pool.idle_count(), pool.live_count());
        }
    }
    std::printf("Lease 析构后：空闲连接数=%zu, 已建连接数=%zu\n", pool.idle_count(), pool.live_count());
    conn.close();
    pool.close_all();
    return 0;
}
