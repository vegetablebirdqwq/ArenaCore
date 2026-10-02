// tools/room_manager_smoke.cpp
// RoomManager 验证：
//   1. 前 6 个玩家进同一个房间，第 7 个自动开新房间（分片）
//   2. tick_all 推进所有房间
//   3. 房间清空后 reap 回收
#include "game/room_manager.h"

#include <chrono>
#include <cstdio>
#include <string>

static int failures = 0;
static void expect(const char* name, bool cond) {
    std::printf("%s: %s\n", cond ? "PASS" : "FAIL", name);
    if (!cond) ++failures;
}

using namespace arena::game;

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    RoomManager mgr;
    auto now = std::chrono::steady_clock::now();

    // 1. 6 个玩家进同一个房间（3v3：前 3 人队 0，后 3 人队 1），第 7 个开新房间
    const std::uint32_t r1 = mgr.join_or_create(1, 0, "p1");
    mgr.join_or_create(2, 0, "p2");
    mgr.join_or_create(3, 0, "p3");
    mgr.join_or_create(4, 1, "p4");
    mgr.join_or_create(5, 1, "p5");
    mgr.join_or_create(6, 1, "p6");
    const std::uint32_t r2 = mgr.join_or_create(7, 0, "p7");

    Room* p1_room = mgr.find_room_of_player(1);
    Room* p6_room = mgr.find_room_of_player(6);
    expect("前 6 人同房", p1_room != nullptr && p6_room != nullptr &&
                          p1_room->id() == r1 && p6_room->id() == r1);
    expect("第 7 人开新房间", r2 != r1 && mgr.room_count() == 2);
    Room* r1p = mgr.find_room(r1);
    Room* r2p = mgr.find_room(r2);
    expect("房间 1 满员", r1p != nullptr && r1p->full());
    expect("房间 2 有 1 人", r2p != nullptr && r2p->player_count() == 1);

    // 2. tick_all 推进（不崩、帧号前进）
    for (int i = 0; i < 60; ++i) {
        now += std::chrono::milliseconds(34);
        mgr.tick_all(now);
    }
    r1p = mgr.find_room(r1);
    r2p = mgr.find_room(r2);
    expect("tick_all 推进两个房间",
           r1p != nullptr && r2p != nullptr && r1p->frame() > 0 && r2p->frame() > 0);

    // 3. 房间 1 满员后开打（kFighting）
    expect("房间 1 满员后开打", r1p != nullptr && r1p->state() == RoomState::kFighting);

    // 4. 清空房间 2 → reap 回收空房间
    if (r2p != nullptr) {
        r2p->remove_player(7);
    }
    now += std::chrono::milliseconds(34);
    mgr.tick_all(now);
    r1p = mgr.find_room(r1);
    const std::uint64_t r1_frame = r1p != nullptr ? r1p->frame() : 0;
    const std::size_t reaped = mgr.reap_idle(r1_frame);
    expect("空房间被回收", reaped >= 1 && mgr.room_count() == 1);
    expect("玩家 7 的反查已清除", mgr.find_room_of_player(7) == nullptr);

    std::printf("\n%s（%d 个失败）\n", failures == 0 ? "全部通过" : "有失败", failures);
    return failures == 0 ? 0 : 1;
}
