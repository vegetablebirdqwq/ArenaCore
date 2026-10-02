#include "game/room.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "game/seq.h"

namespace arena::game {

namespace {

/// 定点距离的平方，用 int64 防止溢出。
/// 坐标范围 ±8000 单位 = ±8000*65536 ≈ ±5.24e8，平方 ≈ 2.7e17，int64 上限 9.2e18，安全。
inline std::int64_t dist_sq_fp(std::int32_t ax, std::int32_t ay,
                               std::int32_t bx, std::int32_t by) noexcept {
    const std::int64_t dx = static_cast<std::int64_t>(ax) - bx;
    const std::int64_t dy = static_cast<std::int64_t>(ay) - by;
    return dx * dx + dy * dy;
}

}  // namespace

void Room::tick(Clock::time_point now) {
    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(now - last_).count();
    last_ = now;

    // ① 夹紧：单次补帧最多追 5 帧（166ms）的欠账，防止死亡螺旋。
    if (elapsed_ms > 5.0 * kFrameMs) {
        acc_ms_ = 0.0;
        last_ = now;
        do_frame();
        return;
    }

    acc_ms_ += elapsed_ms;

    constexpr double kEps = 1e-9;
    while (acc_ms_ + kEps >= kFrameMs) {
        acc_ms_ -= kFrameMs;
        do_frame();
    }
}

void Room::do_frame() {
    ++frame_;

    // 七个阶段，顺序不能错（教程 §2.4）：
    // ① 输入已经在 pending_* 里（业务线程在 tick 前 enqueue）
    // ② 先结算移动（玩家的新位置决定后面一切判定）
    settle_movement();
    // ③ 再结算技能（依赖移动后的位置）
    settle_skills();
    // ④ 重建 AOI 网格（基于最新位置）
    update_aoi();
    // ⑤ 死亡与胜负判定
    settle_death_and_result();
    // ⑥ 生成快照
    build_snapshot();
    // ⑦ 广播增量
    broadcast();
}

void Room::settle_movement() {
    if (pending_moves_.empty()) {
        return;
    }

    for (const MoveInput& in : pending_moves_) {
        const auto it = players_.find(in.player_id);
        if (it == players_.end()) {
            continue;
        }
        Player& p = it->second;
        if (p.state != EntityState::kAlive) {
            continue;
        }

        // 幂等去重：回绕安全比较
        if (in.seq != 0 && !is_newer_seq(in.seq, p.last_cmd_seq)) {
            ++p.dup_dropped;
            continue;
        }
        p.last_cmd_seq = in.seq;

        // 服务端限幅：客户端说"瞬移 10 万单位"也不能信
        const std::int32_t dx = std::clamp(in.dx, -kMaxStepFp, kMaxStepFp);
        const std::int32_t dy = std::clamp(in.dy, -kMaxStepFp, kMaxStepFp);

        p.x = std::clamp(p.x + dx, kWorldMin, kWorldMax);
        p.y = std::clamp(p.y + dy, kWorldMin, kWorldMax);

        last_active_frame_ = frame_;
    }
    pending_moves_.clear();
}

void Room::settle_skills() {
    for (const SkillInput& in : pending_skills_) {
        const auto it = players_.find(in.player_id);
        if (it == players_.end()) {
            continue;
        }
        Player& caster = it->second;

        // ① 施法者必须活着
        if (caster.state != EntityState::kAlive) {
            continue;
        }
        // ② 幂等：技能指令有独立序号空间（和移动分开记）
        if (in.seq != 0 && static_cast<std::uint16_t>(in.seq - caster.last_skill_seq) == 0) {
            ++caster.dup_dropped;
            continue;
        }
        if (in.seq != 0) {
            caster.last_skill_seq = in.seq;
        }
        // ③ 技能 CD —— 纯整数帧比较
        if (frame_ < caster.skill_ready_frame) {
            continue;
        }

        const auto tit = players_.find(in.target_id);
        if (tit == players_.end()) {
            continue;
        }
        Player& target = tit->second;

        // ④ 不能打自己、不能打队友、目标必须活着
        if (target.id == caster.id) {
            continue;
        }
        if (target.team == caster.team) {
            continue;
        }
        if (target.state != EntityState::kAlive) {
            continue;
        }

        // ⑤ 射程校验 —— 反外挂核心：客户端说"我砍他"，服务端自己算距离
        const std::int64_t d2 = dist_sq_fp(caster.x, caster.y, target.x, target.y);
        const std::int64_t range2 = static_cast<std::int64_t>(kSkillRangeFp) * kSkillRangeFp;
        if (d2 > range2) {
            continue;
        }

        // ⑥ 全部校验通过，才真正结算（CD 也是校验通过才扣）
        caster.skill_ready_frame = frame_ + kSkillCooldownFrames;
        target.hp = std::max<std::int32_t>(0, target.hp - caster.atk);

        last_active_frame_ = frame_;
    }
    pending_skills_.clear();
}

void Room::update_aoi() {
    // 一帧只重建一次网格，所有人共用。复用 AoiGrid 成员的内存。
    grid_.clear();
    for (const auto& [id, p] : players_) {
        if (p.state == EntityState::kAlive) {
            grid_.insert(id, p.x, p.y);
        }
    }
    // 给每个活着的玩家算视野集合（九宫格内所有实体），
    // 存进 last_neighbors —— diff_for 就基于它做增量。
    for (auto& [id, p] : players_) {
        if (p.state == EntityState::kAlive) {
            grid_.query_neighbors(p.x, p.y, id, p.last_neighbors);
        } else {
            p.last_neighbors.clear();
        }
    }
}

void Room::settle_death_and_result() {
    // 统一处理死亡（不在伤害循环里删，避免迭代器失效）
    for (auto& [id, p] : players_) {
        if (p.state == EntityState::kAlive && p.hp <= 0) {
            p.state = EntityState::kDead;
            p.hp = 0;
            last_active_frame_ = frame_;
        }
    }

    if (state_ != RoomState::kFighting) {
        return;
    }

    // 胜负判定：某一队全灭
    bool team_alive[2] = {false, false};
    for (const auto& [id, p] : players_) {
        if (p.state == EntityState::kAlive && p.team < 2) {
            team_alive[p.team] = true;
        }
    }
    if (!team_alive[0] || !team_alive[1]) {
        state_ = RoomState::kSettled;
        last_active_frame_ = frame_;
    }
}

}  // namespace arena::game
