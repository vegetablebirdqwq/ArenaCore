// tools/snapshot_smoke.cpp
// 状态同步验证（教程 §6.6）：
//   1. 基线正确性：A 静止、B 移动 → A 收到的 delta 只有 B 的条目（且不含 A 自己）
//   2. 编解码往返：encode → decode → 字段一致
//   3. 字节数对比：紧凑编码 vs 朴素编码（id:uint32/坐标:int32/血量:int32/每字段bool）
#include "game/room.h"
#include "game/snapshot.h"
#include "game/snapshot_codec.h"

#include <cstdint>
#include <cstdio>
#include <vector>

static int failures = 0;
static void expect(const char* name, bool cond) {
    std::printf("%s: %s\n", cond ? "PASS" : "FAIL", name);
    if (!cond) ++failures;
}

using namespace arena::game;
using Clock = std::chrono::steady_clock;
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

    // ---- 收集 A 收到的 delta ----
    std::vector<DeltaFrame> a_deltas;
    {
        Room room;
        room.set_send_fn([&](std::uint32_t pid, std::uint16_t cmd,
                             const std::vector<std::uint8_t>& payload) {
            if (pid == 1 && cmd == kCmdSnapshotDelta) {
                DeltaFrame d;
                if (decode_delta(payload.data(), payload.size(), d)) {
                    a_deltas.push_back(std::move(d));
                }
            }
        });

        room.add_player(mk_player(1, 0, 0, 0));
        room.add_player(mk_player(2, 1, 130 * kFpOne, 0));  // 视野内
        room.add_player(mk_player(3, 0, -130 * kFpOne, 0)); // 视野内

        // 帧 1：所有人入场（都是新进入，全量）
        now = advance_one_frame(now); room.tick(now);
        // 帧 2~10：A 静止，B 每帧移动 2 单位，C 静止
        for (int f = 0; f < 9; ++f) {
            room.enqueue_move(MoveInput{2, static_cast<std::uint16_t>(f + 1),
                                        kFpOne, 0});   // B 每帧移 1 单位
            now = advance_one_frame(now); room.tick(now);
        }
    }

    // ---- 1. 基线正确性 ----
    // 找到「只有 B 变」的那一帧：A 的 delta 里应该只有 B 的条目（id==2），
    // 且绝不含 A 自己（id==1）。
    bool found_delta_with_only_b = false;
    for (const auto& d : a_deltas) {
        bool only_b = !d.entries.empty();
        for (const auto& e : d.entries) {
            if (e.id == 1) only_b = false;   // 不能有 A 自己
            if (e.id != 2) only_b = false;   // 只能有 B
        }
        if (only_b) {
            found_delta_with_only_b = true;
            std::printf("  找到一帧 delta：只有 B 移动，%zu 个条目，frame=%llu\n",
                        d.entries.size(), static_cast<unsigned long long>(d.frame));
            break;
        }
    }
    expect("基线正确性：A 收到的 delta 只有 B（不含 A 自己、不含静止的 C）",
           found_delta_with_only_b);

    // ---- 2. 编解码往返 ----
    {
        DeltaFrame in;
        in.frame = 42;
        DeltaEntry e1;
        e1.id = 7; e1.mask = kFieldPos | kFieldHp;
        e1.x = -1234; e1.y = 567; e1.hp_percent = 88;
        in.entries.push_back(e1);
        DeltaEntry e2;
        e2.id = 300; e2.mask = kFieldFlags; e2.flags = 1;
        in.entries.push_back(e2);

        auto payload = encode_delta(in);
        DeltaFrame out;
        bool ok = decode_delta(payload.data(), payload.size(), out);
        bool match = ok && out.frame == 42 && out.entries.size() == 2 &&
                     out.entries[0].id == 7 &&
                     (out.entries[0].mask & kFieldPos) != 0 &&
                     out.entries[0].x == -1234 && out.entries[0].y == 567 &&
                     out.entries[0].hp_percent == 88 &&
                     out.entries[1].id == 300 && out.entries[1].flags == 1;
        expect("编解码往返一致", match);
    }

    // ---- 3. 字节数对比 ----
    {
        // 紧凑版：10 个实体，都变位置+血量
        DeltaFrame d;
        d.frame = 1;
        for (std::uint32_t i = 1; i <= 10; ++i) {
            DeltaEntry e;
            e.id = i;
            e.mask = kFieldPos | kFieldHp;
            e.x = static_cast<std::int16_t>(i * 10);
            e.y = static_cast<std::int16_t>(i * -5);
            e.hp_percent = static_cast<std::uint8_t>(100 - i);
            d.entries.push_back(e);
        }
        const auto compact = encode_delta(d);

        // 朴素版：id uint32(4) + x int32(4) + y int32(4) + hp int32(4) + 2 bool(2) = 18/实体
        const std::size_t naive = 10 * 18;

        std::printf("  紧凑编码 %zu 字节 vs 朴素编码 %zu 字节 (%.0f%%)\n",
                    compact.size(), naive, 100.0 * compact.size() / naive);
        expect("紧凑编码 < 朴素编码的 50%", compact.size() < naive / 2);
    }

    std::printf("\n%s（%d 个失败）\n", failures == 0 ? "全部通过" : "有失败", failures);
    return failures == 0 ? 0 : 1;
}
