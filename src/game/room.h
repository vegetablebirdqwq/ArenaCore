// game/room.h
// 房间：固定帧累加器 + "收包 → 移动 → 技能 → 死亡/胜负 → AOI → 快照 → 广播"
// 七阶段（教程第 5 篇 §2.4）。
//
// 设计要点：
//  - tick() 只管时间（累加器补帧），do_one_frame_helper() 只管逻辑 ——
//    补帧时每一帧都要完整跑七阶段，不能跳（教程 §2.4 的拆法）。
//  - 收包阶段只入队不碰世界状态；结算按 (player_id, seq) 排序，保证确定性。
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "game/snapshot.h"
#include "game/world.h"

namespace arena::game {

class AoiGrid;  // 前置声明；成员是 unique_ptr，析构在 room.cpp 里见到完整定义即可

using Clock = std::chrono::steady_clock;

// 逻辑帧间隔（毫秒）。30Hz => 1000/30 = 33.3333...
// 不能写 33：30 帧会偏快 1%（教程 §1.3 的帧率漂移）。
inline constexpr double kFrameMs = 1000.0 / 30.0;

// 单帧 tick 的绝对上限。不是"目标"，是"熔断阈值"。
inline constexpr double kFrameBudgetMs = 12.0;

// ---- 战斗规则常量（教程 §5）----
// CD 记"帧到期时刻"而非浮点秒数：帧计数是整数，永远精确，可序列化、可重放。
inline constexpr std::uint16_t kSkillCooldownFrames = 30;      // 1 秒
inline constexpr std::uint32_t kSkillRangeFp = 300 * kFpOne;   // 技能射程 300 单位（定点）
inline constexpr std::int32_t kMaxStepFp = 2 * kFpOne;         // 每帧最多位移 2 单位（服务端限幅）

// ---- 逻辑层 → 客户端的广播命令号（真正的项目里和客户端共用一张协议表）----
inline constexpr std::uint16_t kCmdSnapshotFull  = 0x0301;
inline constexpr std::uint16_t kCmdSnapshotDelta = 0x0302;
inline constexpr std::uint16_t kCmdAoiEnter      = 0x0303;
inline constexpr std::uint16_t kCmdAoiLeave      = 0x0304;
inline constexpr std::uint16_t kCmdRoomMeta      = 0x0305;

/// 移动输入。注意：**没有任何"结果"字段** —— 客户端只能提交意图，
/// 服务端负责限幅、校验和判定（服务端权威，教程 §5.1）。
struct MoveInput {
    std::uint32_t player_id = 0;
    std::int32_t  dx = 0;            // 定点 Q16.16，"这一帧想往哪个方向走多少"
    std::int32_t  dy = 0;
    std::uint16_t seq = 0;           // 客户端指令序号，幂等去重用（回绕安全比较在 seq.h）
};

/// 技能输入。同样没有伤害值、没有坐标 —— 只有 skill_id + target_id。
/// 玩家说"我想砍 3 号"，服务器自己查"3 号在不在你面前、你技能好没好"。
struct SkillInput {
    std::uint32_t player_id = 0;
    std::uint16_t skill_id = 0;
    std::uint32_t target_id = 0;
    std::uint16_t seq = 0;
};

enum class RoomState : std::uint8_t {
    kWaiting = 0,   // 等人
    kFighting = 1,
    kSettled = 2,   // 已出胜负，等客户端看完结算动画
    kClosed = 3,    // 可以回收
};

/// 发送回调：由 logic 进程的发送层实现（可能走内部通道转发给 gateway）。
using SendFn = std::function<void(std::uint32_t player_id,
                                  std::uint16_t cmd,
                                  const std::vector<std::uint8_t>& payload)>;

class Room {
public:
    /// 便捷构造：id = 0、发送回调为空操作。
    /// 仅给"只验证固定帧"的 smoke 用（tools/logic_frame_smoke.cpp）；
    /// 正式房间一律走下面的完整构造。
    Room();

    /// 完整构造。send 由调用方（逻辑线程/发送层）提供。
    Room(std::uint32_t id, SendFn send);

    ~Room();

    void add_player(std::uint32_t player_id, std::uint8_t team, std::string name);
    void remove_player(std::uint32_t player_id);
    bool full() const noexcept { return players_.size() >= 6; }

    /// 收包：只记录输入，绝不碰世界状态。由网络层/逻辑线程调用。
    void submit_move(const MoveInput& in);
    void submit_skill(const SkillInput& in);

    void tick(Clock::time_point now);

    // ---- 重连 ----
    void on_reconnect(std::uint32_t player_id);

    /// 发送回调注入（便捷路径）。网络集成 smoke（tools/game_server_smoke.cpp）
    /// 用它在 Room 构造之后接线；正式用法是直接走构造函数。
    void set_send_fn(SendFn fn) { send_ = std::move(fn); }

    std::uint32_t id() const noexcept { return id_; }
    RoomState state() const noexcept { return state_; }
    std::uint64_t frame() const noexcept { return frame_; }
    std::uint64_t last_active_frame() const noexcept { return last_active_frame_; }

    // 只读访问器，给 smoke / 测试用
    const std::unordered_map<std::uint32_t, Player>& players() const noexcept { return players_; }
    std::size_t player_count() const noexcept { return players_.size(); }
    Player* find_player(std::uint32_t player_id);
    std::uint64_t broadcast_persons() const noexcept { return broadcast_persons_; }
    std::uint64_t full_broadcast_persons() const noexcept { return full_broadcast_persons_; }

private:
    void do_one_frame_helper();      // 七个阶段的串行执行体（tick 里补一帧调一次）
    void collect_inputs();           // ① 收包落定 + 确定性排序
    void settle_movement();          // ② 移动结算
    void settle_skills();            // ③ 技能结算
    void settle_death_and_result();  // ④ 死亡与胜负
    void update_aoi();               // ⑤ AOI 进出
    void build_snapshot();           // ⑥ 快照
    void broadcast();                // ⑦ 广播

    DeltaFrame diff_for(std::uint32_t viewer_id, bool force_full);
    void send_enter_packet(std::uint32_t viewer_id, std::uint32_t entity_id);
    void send_leave_packet(std::uint32_t viewer_id, std::uint32_t entity_id);
    void send_room_meta(std::uint32_t player_id, std::uint64_t frame);
    void count_broadcast(std::uint32_t viewer_id, std::size_t entry_count, std::size_t byte_count);

    std::uint32_t id_ = 0;
    RoomState state_ = RoomState::kWaiting;
    std::uint64_t frame_ = 0;
    std::uint64_t last_active_frame_ = 0;
    Clock::time_point last_ = Clock::now();
    double acc_ms_ = 0.0;

    std::unordered_map<std::uint32_t, Player> players_;

    std::vector<MoveInput> pending_moves_;
    std::vector<SkillInput> pending_skills_;

    Snapshot snapshot_;
    std::unique_ptr<AoiGrid> grid_;   // 每帧 clear 复用，避免反复分配
    SendFn send_;

    // 埋点：本房间累计广播"人次"和字节数（教程 §4.6 用它算 AOI 降幅）
    std::uint64_t broadcast_persons_ = 0;
    std::uint64_t broadcast_bytes_ = 0;
    std::uint64_t full_broadcast_persons_ = 0;
};

}  // namespace arena::game
