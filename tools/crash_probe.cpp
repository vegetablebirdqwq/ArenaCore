// tools/crash_probe.cpp —— 最小复现：只构造 Room + 注入 send 回调，不碰网络
#include "game/room.h"
#include <cstdio>

int main() {
    std::fprintf(stderr, "[probe] step 0\n");
    arena::game::Room room;
    std::fprintf(stderr, "[probe] step 1: Room 构造完成\n");
    room.set_send_fn([](std::uint32_t, std::uint16_t, const std::vector<std::uint8_t>&) {
        // 空回调
    });
    std::fprintf(stderr, "[probe] step 2: send 回调注入完成\n");

    // 加一个玩家，跑一帧
    arena::game::Player p;
    p.id = 1;
    p.team = 0;
    room.add_player(std::move(p));
    std::fprintf(stderr, "[probe] step 3: 玩家加入\n");

    room.tick(arena::game::Clock::now());
    std::fprintf(stderr, "[probe] step 4: tick 完成，frame=%llu\n",
                 static_cast<unsigned long long>(room.frame()));
    return 0;
}
