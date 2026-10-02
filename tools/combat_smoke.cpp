// tools/combat_smoke.cpp
// 战斗结算验证（教程 §5.4 的 4 个自己验证 + 2 个附赠）：
//   1. 射程边界：恰好 300 单位命中、300+1 单位不命中（定点整数比较的边界）
//   2. CD 精度：连续 100 帧发技能，释放间隔严格 30 帧（无 ±1 偏移）
//   3. 反外挂：服务端权威 —— 伪造位移被限幅、伪造攻击（超射程/打队友/打自己）全部被拒
//   4. 同帧互杀确定性：A、B 各 120 血互杀，跑 100 次 × 两种入队顺序，结果完全一致
//   5. （附赠）快照编解码 encode → decode 往返一致（教程 §6.6 验收项）
//   6. （附赠）is_newer_seq 回绕三断言（教程 §7.3）
#include "game/room.h"
#include "game/seq.h"
#include "game/snapshot_codec.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using arena::game::Clock;
using arena::game::DeltaEntry;
using arena::game::DeltaFrame;
using arena::game::EntityState;
using arena::game::kFpOne;
using arena::game::MoveInput;
using arena::game::Player;
using arena::game::Room;
using arena::game::RoomState;
using arena::game::SkillInput;

namespace {

int failures = 0;

void expect(const char* name, bool cond) {
    std::printf("%s: %s\n", cond ? "PASS" : "FAIL", name);
    if (!cond) {
        ++failures;
    }
}

/// 测试夹具：6 人满房（add_player 满 6 人 → kFighting）+ 精确单帧推进 + 可写 Player 访问。
struct TestRoom {
    std::uint64_t packets = 0;   // 发送回调埋点：顺便验证七阶段广播路径不崩
    std::uint64_t bytes = 0;
    Room room;

    TestRoom() : room(1, [this](std::uint32_t, std::uint16_t,
                                const std::vector<std::uint8_t>& payload) {
        ++packets;
        bytes += payload.size();
    }) {
        for (std::uint32_t i = 0; i < 6; ++i) {
            room.add_player(100 + i, static_cast<std::uint8_t>(i / 3), "p" + std::to_string(i));
        }
    }

    /// 可写访问。players() 返回 const 引用，测试摆位/改血要用 const_cast ——
    /// 这是有意为之：生产接口**不该**开放"任意传送/改血"，那会是反外挂的洞
    /// （教程 §8 也这么干，并明确要求把测试 hack 的原因写清楚）。
    Player& player(std::uint32_t id) {
        auto it = room.players().find(id);
        auto& p = const_cast<Player&>(it->second);
        return p;
    }

    /// 精确推进一帧。33334µs = 33.334ms，比 kFrameMs(33.3333…) 略大，保证每次
    /// tick 恰好补 1 帧。教程 §8 的 duration_cast(1000/30) 写法在 MSVC steady_clock
    /// （100ns 节拍）下会被截断成 33.3333ms < kFrameMs，累积漂移偏慢 ——
    /// 本 smoke 要断言精确的释放帧号，不能用那种写法。
    void advance_one_frame() {
        now_ += std::chrono::microseconds(33334);
        room.tick(now_);
    }

private:
    Clock::time_point now_ = Clock::now();
};

SkillInput make_skill(std::uint32_t caster, std::uint32_t target, std::uint16_t seq) {
    SkillInput si;
    si.player_id = caster;
    si.skill_id = 1;
    si.target_id = target;
    si.seq = seq;
    return si;
}

// ---- 1. 射程边界（教程 §5.4-1）----
void test_range_boundary() {
    {
        // 恰好 300 单位：d² == r²，边界是闭的，必须命中
        TestRoom tr;
        tr.player(100).x = 0; tr.player(100).y = 0;                 // A 在原点
        tr.player(103).x = 300 * kFpOne; tr.player(103).y = 0;      // B 恰好 300 单位
        tr.room.submit_skill(make_skill(100, 103, 1));
        tr.advance_one_frame();
        expect("射程边界：恰好 300 单位命中（B 掉 120 血）", tr.player(103).hp == 1000 - 120);
        expect("射程边界：命中后进入 CD", tr.player(100).skill_ready_frame == 30);
    }
    {
        // 300+1 单位：d² > r²，必须不命中（防止作弊者卡边界）
        TestRoom tr;
        tr.player(100).x = 0; tr.player(100).y = 0;
        tr.player(103).x = 300 * kFpOne + 1; tr.player(103).y = 0;
        tr.room.submit_skill(make_skill(100, 103, 1));
        tr.advance_one_frame();
        expect("射程边界：300+1 单位不命中（B 满血）", tr.player(103).hp == 1000);
        expect("射程边界：不命中不扣 CD（可立即再试）", tr.player(100).skill_ready_frame == 0);
    }
}

// ---- 2. CD 精度（教程 §5.4-2）----
void test_cd_precision() {
    TestRoom tr;
    tr.player(100).x = 0; tr.player(100).y = 0;
    tr.player(103).x = 100 * kFpOne; tr.player(103).y = 0;   // 射程内

    // 连续 100 帧每帧提交一次技能，记录 B 掉血的帧号 = 技能释放帧号。
    std::vector<int> cast_frames;
    int prev_hp = tr.player(103).hp;
    for (int f = 0; f < 100; ++f) {
        tr.room.submit_skill(make_skill(100, 103, static_cast<std::uint16_t>(f + 1)));
        tr.advance_one_frame();
        const int hp_now = tr.player(103).hp;
        if (hp_now < prev_hp) {
            cast_frames.push_back(f);
        }
        prev_hp = hp_now;
    }

    // 教程 §5.4 原文说"100 帧正好 3 次（第 0、30、60 帧）"，但那少算了一刀：
    // 第 90 帧恰好是第 3 个 CD 周期结束（0 + 3×30），100 帧内必然还能放第 4 刀。
    // 这里断言的是真正的精度属性：释放帧严格为 {0,30,60,90}，间隔整 30 帧、无 ±1。
    const std::vector<int> expected = {0, 30, 60, 90};
    expect("CD 精度：100 帧内释放帧 == {0,30,60,90}（间隔严格 30 帧）", cast_frames == expected);

    // 顺带验证 B 恰好吃了 4 刀
    expect("CD 精度：B 恰好掉 4×120 血", tr.player(103).hp == 1000 - 4 * 120);
}

// ---- 3. 反外挂（教程 §5.4-3）----
void test_anti_cheat() {
    TestRoom tr;

    // 服务端权威：SkillInput 里根本没有坐标字段（客户端"伪造位置"在协议层就
    // 无处安放）。这里验证两层防线：
    //   (a) 伪造位移：客户端发 dx=100 单位/帧想"瞬移"，服务端 clamp 到 2 单位/帧；
    //   (b) 伪造攻击：A、B 实际相距 400 单位（超射程），服务端用自己的权威坐标
    //       判射程 —— 攻击必须失败，且失败不扣 CD。
    tr.player(100).x = 0; tr.player(100).y = 0;
    tr.player(103).x = 400 * kFpOne; tr.player(103).y = 0;

    MoveInput forged;
    forged.player_id = 100;
    forged.dx = 100 * kFpOne;   // 客户端声称一帧走 100 单位
    forged.dy = 0;
    forged.seq = 1;
    tr.room.submit_move(forged);
    tr.advance_one_frame();
    expect("反外挂：伪造位移被限幅（只走 2 单位，不是 100）", tr.player(100).x == 2 * kFpOne);

    tr.room.submit_skill(make_skill(100, 103, 1));
    tr.advance_one_frame();
    expect("反外挂：超射程伪造攻击不造成伤害", tr.player(103).hp == 1000);
    expect("反外挂：被拒的攻击不消耗 CD", tr.player(100).skill_ready_frame == 0);

    // 目标校验：打队友（贴脸也不行）
    tr.player(101).x = 10 * kFpOne; tr.player(101).y = 0;   // 队友 101 就站在旁边
    tr.room.submit_skill(make_skill(100, 101, 2));
    tr.advance_one_frame();
    expect("反外挂：打队友被拒绝", tr.player(101).hp == 1000);

    // 目标校验：打自己
    tr.room.submit_skill(make_skill(100, 100, 3));
    tr.advance_one_frame();
    expect("反外挂：打自己被拒绝", tr.player(100).hp == 1000);
}

// ---- 4. 同帧互杀确定性（教程 §5.4-4 / §2.5-2）----
void test_mutual_kill_determinism() {
    bool deterministic = true;
    for (int run = 0; run < 100; ++run) {
        TestRoom tr;
        // 四个旁观者先躺下：否则 A、B 死后各自还有队友活着，房间不会结算
        for (std::uint32_t i = 0; i < 6; ++i) {
            if (i != 0 && i != 3) {
                Player& p = tr.player(100 + i);
                p.hp = 0;
                p.state = EntityState::kDead;
            }
        }
        // A(100) team0 与 B(103) team1 各 120 血（正好一刀），互相在射程内
        tr.player(100).x = 0; tr.player(100).y = 0; tr.player(100).hp = 120;
        tr.player(103).x = 100 * kFpOne; tr.player(103).y = 0; tr.player(103).hp = 120;

        // 两种入队顺序交替（collect_inputs 会按 player_id 排序，结果必须一致）
        if ((run & 1) == 0) {
            tr.room.submit_skill(make_skill(100, 103, 1));
            tr.room.submit_skill(make_skill(103, 100, 1));
        } else {
            tr.room.submit_skill(make_skill(103, 100, 1));
            tr.room.submit_skill(make_skill(100, 103, 1));
        }
        tr.advance_one_frame();

        const Player& a = tr.room.players().at(100);
        const Player& b = tr.room.players().at(103);
        const bool ok = a.hp == 0 && a.state == EntityState::kDead &&
                        b.hp == 0 && b.state == EntityState::kDead &&
                        tr.room.state() == RoomState::kSettled;
        if (!ok) {
            deterministic = false;
            std::printf("  第 %d 次不一致：A(hp=%d,state=%d) B(hp=%d,state=%d) room=%d\n",
                        run, a.hp, static_cast<int>(a.state), b.hp,
                        static_cast<int>(b.state), static_cast<int>(tr.room.state()));
            break;
        }
    }
    expect("同帧互杀：100 次 × 2 种入队顺序结果一致（双双阵亡、房间结算）", deterministic);
}

// ---- 5. 附赠：快照编解码往返（教程 §6.6 验收项：负数坐标/0 血/满血/id=0）----
void test_snapshot_codec_roundtrip() {
    DeltaFrame d;
    d.frame = 12345;
    DeltaEntry e1;
    e1.id = 0;
    e1.mask = static_cast<std::uint8_t>(arena::game::kFieldPos |
                                        arena::game::kFieldHp |
                                        arena::game::kFieldFlags);
    e1.x = -32767; e1.y = 300; e1.hp_percent = 0; e1.flags = 0;
    DeltaEntry e2;
    e2.id = 7; e2.mask = static_cast<std::uint8_t>(arena::game::kFieldHp);
    e2.hp_percent = 100;
    DeltaEntry e3;
    e3.id = 300; e3.mask = static_cast<std::uint8_t>(arena::game::kFieldPos);
    e3.x = -5; e3.y = -300;
    d.entries = {e1, e2, e3};

    const std::vector<std::uint8_t> payload = arena::game::encode_delta(d);
    DeltaFrame back;
    const bool ok = arena::game::decode_delta(payload.data(), payload.size(), back);
    expect("快照编解码：往返解析成功", ok);
    if (ok) {
        bool same = back.frame == d.frame && back.entries.size() == d.entries.size();
        if (same) {
            for (std::size_t i = 0; i < d.entries.size(); ++i) {
                const auto& a = d.entries[i];
                const auto& b = back.entries[i];
                if (a.id != b.id || a.mask != b.mask || a.x != b.x || a.y != b.y ||
                    a.hp_percent != b.hp_percent || a.flags != b.flags) {
                    same = false;
                    break;
                }
            }
        }
        expect("快照编解码：逐字段一致（负数坐标/0 血/满血/id=0）", same);
    }
}

// ---- 6. 附赠：seq 回绕三断言（教程 §7.3）----
void test_seq_wraparound() {
    expect("seq 回绕：is_newer_seq(0, 65535) == true", arena::game::is_newer_seq(0, 65535));
    expect("seq 回绕：is_newer_seq(65535, 0) == false", !arena::game::is_newer_seq(65535, 0));
    expect("seq 回绕：is_newer_seq(100, 100) == false", !arena::game::is_newer_seq(100, 100));
}

}  // namespace

int main() {
    test_range_boundary();
    test_cd_precision();
    test_anti_cheat();
    test_mutual_kill_determinism();
    test_snapshot_codec_roundtrip();
    test_seq_wraparound();

    std::printf("\n%s（%d 个失败）\n", failures == 0 ? "全部通过" : "有失败", failures);
    return failures == 0 ? 0 : 1;
}
