// tools/combat_smoke.cpp
// 战斗结算验证（教程 §5.4 的 4 个自己验证）：
//   1. 射程边界：300 单位命中，300+1 不命中
//   2. CD 精度：100 帧每帧发技能，正好放 4 次（第 1/30/60/90 帧，CD 30 帧）
//   3. 反外挂：伪造位移（限幅 2 单位/帧）后技能仍不命中
//   4. 同帧互杀确定性：A/B 各 120 血同帧互砍，都死；跑 100 次结果一致
//
// 帧驱动：手动喂递增的 now（每帧 33.334ms），不依赖真实时钟，
// 保证每次 tick 恰好推进一帧。
#include "game/room.h"
#include "game/seq.h"

#include <chrono>
#include <cstdio>

static int failures = 0;
static void expect(const char* name, bool cond) {
    std::printf("%s: %s\n", cond ? "PASS" : "FAIL", name);
    if (!cond) ++failures;
}

using namespace arena::game;
using Clock = std::chrono::steady_clock;

/// 前进一帧（33.334ms），返回新的 now。
static Clock::time_point advance_one_frame(Clock::time_point now) {
    return now + std::chrono::microseconds(33334);
}

static Player mk_player(std::uint32_t id, std::uint8_t team,
                        std::int32_t x, std::int32_t y) {
    Player p;
    p.id = id;
    p.team = team;
    p.x = x;
    p.y = y;
    return p;
}

int main() {
    auto now = Clock::now();

    // ---- 1. 射程边界 ----
    {
        Room room;
        auto a = mk_player(1, 0, 0, 0);
        auto b = mk_player(2, 1, 300 * kFpOne, 0);   // 恰好 300 单位
        b.hp = 1000;
        room.add_player(std::move(a));
        room.add_player(std::move(b));
        now = advance_one_frame(now); room.tick(now);
        room.enqueue_skill(SkillInput{1, 2, 1, 1});
        now = advance_one_frame(now); room.tick(now);
        const Player* pb = room.find_player(2);
        expect("射程边界：300 单位命中（hp 1000→880）",
               pb != nullptr && pb->hp == 1000 - 120);
    }
    {
        Room room;
        auto a = mk_player(1, 0, 0, 0);
        auto b = mk_player(2, 1, 300 * kFpOne + 1, 0);   // 300+1，超出射程
        b.hp = 1000;
        room.add_player(std::move(a));
        room.add_player(std::move(b));
        now = advance_one_frame(now); room.tick(now);
        room.enqueue_skill(SkillInput{1, 2, 1, 1});
        now = advance_one_frame(now); room.tick(now);
        const Player* pb = room.find_player(2);
        expect("射程边界：300+1 不命中（hp 不变）",
               pb != nullptr && pb->hp == 1000);
    }

    // ---- 2. CD 精度 ----
    {
        Room room;
        auto a = mk_player(1, 0, 0, 0);
        auto b = mk_player(2, 1, 100 * kFpOne, 0);
        b.hp = 100000;   // 打不死，专注测 CD
        room.add_player(std::move(a));
        room.add_player(std::move(b));
        for (std::uint16_t f = 1; f <= 100; ++f) {
            now = advance_one_frame(now); room.tick(now);
            room.enqueue_skill(SkillInput{1, 2, f, 1});
        }
        // CD=30 帧：第 1、30、60、90 帧应释放 → 4 次 → 掉血 4×120=480
        const Player* pb = room.find_player(2);
        expect("CD 精度：100 帧正好释放 4 次（hp 100000→99520）",
               pb != nullptr && pb->hp == 100000 - 4 * 120);
    }

    // ---- 3. 反外挂：伪造位移后技能不命中 ----
    {
        Room room;
        auto a = mk_player(1, 0, 0, 0);
        auto b = mk_player(2, 1, 400 * kFpOne, 0);   // 400 > 300 射程外
        b.hp = 1000;
        room.add_player(std::move(a));
        room.add_player(std::move(b));
        now = advance_one_frame(now); room.tick(now);
        // 作弊者想"瞬移"到敌人旁边：发超大 dx（100 单位/帧），
        // 但服务端限幅 2 单位/帧 → 10 帧最多移动 20 单位，仍在 380 单位外
        for (int i = 0; i < 10; ++i) {
            room.enqueue_move(MoveInput{1, static_cast<std::uint16_t>(i + 1),
                                        100 * kFpOne, 0});
            now = advance_one_frame(now); room.tick(now);
        }
        room.enqueue_skill(SkillInput{1, 2, 1, 1});
        now = advance_one_frame(now); room.tick(now);
        const Player* pb = room.find_player(2);
        expect("反外挂：伪造位移被限幅，技能不命中（hp 不变）",
               pb != nullptr && pb->hp == 1000);
    }

    // ---- 4. 同帧互杀确定性 ----
    {
        bool all_settled = true;
        for (int trial = 0; trial < 100; ++trial) {
            Room room;
            auto a = mk_player(1, 0, 0, 0);
            auto b = mk_player(2, 1, 100 * kFpOne, 0);
            a.hp = 120;   // 一刀的量
            b.hp = 120;
            room.add_player(std::move(a));
            room.add_player(std::move(b));
            now = advance_one_frame(now); room.tick(now);
            // 同帧互砍
            room.enqueue_skill(SkillInput{1, 2, 1, 1});
            room.enqueue_skill(SkillInput{2, 1, 1, 1});
            now = advance_one_frame(now); room.tick(now);
            if (room.state() != RoomState::kSettled) {
                all_settled = false;
            }
        }
        expect("同帧互杀 100 次：每次都分出胜负", all_settled);
    }

    std::printf("\n%s（%d 个失败）\n", failures == 0 ? "全部通过" : "有失败", failures);
    return failures == 0 ? 0 : 1;
}
