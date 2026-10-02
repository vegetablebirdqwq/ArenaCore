#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include "game/aoi.h"
#include "game/snapshot.h"
#include "game/world.h"

namespace arena::game {

using Clock = std::chrono::steady_clock;

// 逻辑帧间隔，单位毫秒。30Hz => 1000/30 = 33.3333...
inline constexpr double kFrameMs = 1000.0 / 30.0;

// 单帧 tick 的绝对上限。不是"目标"，是"熔断阈值"。
inline constexpr double kFrameBudgetMs = 12.0;

// ---- 战斗规则常量（定点数，见 world.h kFpOne）----
inline constexpr std::int32_t kSkillRangeFp = 300 * kFpOne;      // 技能射程 300 单位
inline constexpr std::uint64_t kSkillCooldownFrames = 30;        // 技能 CD = 30 帧 = 1 秒
inline constexpr std::int32_t kMaxStepFp = 2 * kFpOne;           // 每帧最多移动 2 单位

enum class RoomState : std::uint8_t {
    kFighting = 0,   // 对局进行中
    kSettled = 1,    // 已分出胜负
};

/// 客户端输入：移动指令。只表达"意图方向"，服务端负责限幅。
struct MoveInput {
    std::uint32_t player_id = 0;
    std::uint16_t seq = 0;
    std::int32_t dx = 0;   // 定点，客户端给的，不可信，服务端 clamp
    std::int32_t dy = 0;
};

/// 客户端输入：技能指令。只有 skill_id + target_id，没有伤害值 —— 服务端权威。
struct SkillInput {
    std::uint32_t player_id = 0;
    std::uint32_t target_id = 0;
    std::uint16_t seq = 0;
    std::uint8_t skill_id = 0;
};

// ---- 状态同步命令号（外层包 cmd 字段）----
inline constexpr std::uint16_t kCmdSnapshotDelta = 0x0101;
inline constexpr std::uint16_t kCmdSnapshotFull = 0x0102;

class Room {
public:
    void tick(Clock::time_point now);

    std::uint64_t frame() const noexcept { return frame_; }
    RoomState state() const noexcept { return state_; }

    /// 业务层喂输入（网络线程/逻辑线程调用，tick 前入队）。
    void enqueue_move(MoveInput in) { pending_moves_.push_back(in); }
    void enqueue_skill(SkillInput in) { pending_skills_.push_back(in); }

    /// 添加一个玩家（开房间时）。
    void add_player(Player p) { players_.emplace(p.id, std::move(p)); }

    /// 只读访问玩家（验证/快照用）。
    const Player* find_player(std::uint32_t id) const {
        const auto it = players_.find(id);
        return it == players_.end() ? nullptr : &it->second;
    }

    std::size_t player_count() const noexcept { return players_.size(); }

    // ---- 状态同步（教程 §6）----
    /// 发送回调：业务层注入（网络层负责真正发出去）。
    using SendFn = std::function<void(std::uint32_t player_id, std::uint16_t cmd,
                                      const std::vector<std::uint8_t>& payload)>;
    void set_send_fn(SendFn fn) { send_ = std::move(fn); }

    /// 玩家重连：清基线 + 重算视野 + 发全量快照。
    void on_reconnect(std::uint32_t player_id);

private:
    void do_frame();

    // ---- 七个阶段（严格顺序，见教程 §2.4）----
    void settle_movement();
    void settle_skills();
    void update_aoi();
    void settle_death_and_result();
    void build_snapshot();
    void broadcast();

    /// 生成某玩家视野内的增量帧（force_full = 全量基线）。
    DeltaFrame diff_for(std::uint32_t viewer_id, bool force_full);

    Clock::time_point last_ = Clock::now();
    double acc_ms_ = 0.0;
    std::uint64_t frame_ = 0;
    std::uint64_t last_active_frame_ = 0;

    RoomState state_ = RoomState::kFighting;

    std::unordered_map<std::uint32_t, Player> players_;
    std::vector<MoveInput> pending_moves_;
    std::vector<SkillInput> pending_skills_;
    AoiGrid grid_;
    SendFn send_;              // 状态同步的发送出口（业务层注入）
    std::uint64_t broadcast_count_ = 0;   // 埋点：累计广播人次
    std::uint64_t broadcast_bytes_ = 0;   // 埋点：累计广播字节
};

}  // namespace arena::game
