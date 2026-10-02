// game/room.cpp
// 房间的全部逻辑层方法集中在此（教程第 5 篇把 tick/combat/aoi/snapshot/lifecycle
// 拆成多个 .cpp，本项目合并成一个文件：阶段之间互相调用频繁，拆开反而要在
// header 暴露大量私有方法；逻辑层一共就一个房间类，单文件更直白）。
#include "game/room.h"

#include <algorithm>
#include <iterator>
#include <utility>

#include "game/aoi.h"
#include "game/seq.h"
#include "game/snapshot_codec.h"
#include "net/codec.h"

namespace arena::game {

namespace {

/// 定点距离的平方，用 int64 防止溢出。
/// 坐标范围 ±8000 单位 = ±8000*65536 ≈ ±5.24e8，平方 ≈ 2.7e17，int64 上限 9.2e18，安全。
/// 为什么用 int64 而不是 double：确定性和溢出都更好控制（教程 §5.3）。
inline std::int64_t dist_sq_fp(std::int32_t ax, std::int32_t ay,
                               std::int32_t bx, std::int32_t by) noexcept {
    const std::int64_t dx = static_cast<std::int64_t>(ax) - bx;
    const std::int64_t dy = static_cast<std::int64_t>(ay) - by;
    return dx * dx + dy * dy;
}

}  // namespace

// ---- 生命周期（教程 §2.4）----

Room::Room() : Room(0, [](std::uint32_t, std::uint16_t, const std::vector<std::uint8_t>&) {}) {}

Room::Room(std::uint32_t id, SendFn send)
    : id_(id), grid_(std::make_unique<AoiGrid>()), send_(std::move(send)) {
    snapshot_.entries.reserve(8);
    pending_moves_.reserve(64);
    pending_skills_.reserve(16);
}

Room::~Room() = default;

void Room::add_player(std::uint32_t player_id, std::uint8_t team, std::string name) {
    Player p;
    p.id = player_id;
    p.team = team;
    p.name = std::move(name);

    // 出生点必须在这里设好（教程 §2.4 的教训：别留到测试里用 const_cast 补）。
    // 3v3 两端出生、距离 600（大于射程 300），有一个"走进射程"的过渡期；
    // y 按队伍内序号错开，避免三个人重叠。
    p.x = (team == 0) ? (-300 * kFpOne) : (300 * kFpOne);
    p.y = static_cast<std::int32_t>(players_.size() % 3) * 50 * kFpOne;

    players_.emplace(player_id, std::move(p));

    // 人满了就开打。这里用 6 而不是"双方各 3 人"的判定：
    // 队伍人数校验属于匹配层（match 进程）的职责，逻辑层信任进来的这支队伍 ——
    // 如果这里再校验一遍，匹配规则一改就要改两个地方。
    if (players_.size() >= 6) {
        state_ = RoomState::kFighting;
    }
    last_active_frame_ = frame_;
}

void Room::remove_player(std::uint32_t player_id) {
    const auto it = players_.find(player_id);
    if (it == players_.end()) {
        return;
    }
    // 注意：这里只是把人从房间移除（客户端关闭/被踢）。
    // 断线不应该调它 —— 断线的人还在场上，只是 online = false。
    // 这两件事混起来处理，会导致"队友一掉线就少一个战斗力"。
    players_.erase(it);
    last_active_frame_ = frame_;
    // 人少了不代表比赛结束，交给 settle_death_and_result 判全灭：
    // 如果有人退了导致某一队一个人都没有，下一帧的胜负判定会立刻
    // 把房间推进 kSettled，这是我们要的。
}

Player* Room::find_player(std::uint32_t player_id) {
    const auto it = players_.find(player_id);
    return it == players_.end() ? nullptr : &it->second;
}

// ---- 时间驱动（教程 §2.4）----

void Room::tick(Clock::time_point now) {
    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(now - last_).count();
    last_ = now;

    // ① 夹紧：单次补帧最多追 5 帧（166ms）的欠账。
    //    如果不夹，一次 GC/换页/调度抖动让 elapsed 变成 2 秒，
    //    while 循环要连补 60 帧，而这 60 帧又要花掉 >2 秒 ——
    //    欠账越补越多，这就是"死亡螺旋"。宁可把时间丢掉，
    //    （客户端看到一次卡顿），也不能把服务器拖死。
    if (elapsed_ms > 5.0 * kFrameMs) {
        acc_ms_ = 0.0;
        last_ = now;
        do_one_frame_helper();   // 至少推进一帧，保持心跳
        return;
    }

    acc_ms_ += elapsed_ms;

    // ② 补帧：欠够一帧就补一帧，可能补多帧。
    //    注意这里的比较是 double，33.333 这种数不能用 == 判等。
    constexpr double kEps = 1e-9;
    while (acc_ms_ + kEps >= kFrameMs) {
        acc_ms_ -= kFrameMs;
        do_one_frame_helper();
    }
}

// 七个阶段的串行执行体。顺序是数据依赖逼出来的（教程 §2.3）：
//   技能判定用移动后的位置 → 死亡判定要等所有伤害算完 → AOI 按死亡后的实体算
//   → 快照发给 AOI 算出的可见集合 → 广播只负责发。
// 补帧时每一帧都要完整跑一遍，不能跳（教程 §2.4 的拆法意义就在这）。
void Room::do_one_frame_helper() {
    if (state_ == RoomState::kClosed) {
        return;   // 已回收，tick 是空操作
    }
    if (state_ == RoomState::kWaiting) {
        // 等待期没有任何逻辑可跑，但帧号照样推进：
        // 帧号是逻辑时间，不因为"没开打"就停下来。
        ++frame_;
        return;
    }

    collect_inputs();
    settle_movement();
    settle_skills();
    settle_death_and_result();
    update_aoi();
    build_snapshot();
    broadcast();

    ++frame_;
}

void Room::collect_inputs() {
    // 入队动作在 submit_move/submit_skill 里已做完（只入队、不碰世界）。
    // 这里做的是"本帧输入落定 + 确定性排序"：
    // 不管包以什么顺序到达，结算顺序永远一致（先 player_id 再 seq，两个整数键）。
    // 没有这一步，"谁先结算"就取决于包的到达顺序 —— 同一帧互杀时谁拿人头
    // 就变成网络决定的（教程 §2.2 / §2.5 的乱序实验）。
    std::sort(pending_moves_.begin(), pending_moves_.end(),
              [](const MoveInput& a, const MoveInput& b) {
                  if (a.player_id != b.player_id) return a.player_id < b.player_id;
                  return a.seq < b.seq;
              });
    std::sort(pending_skills_.begin(), pending_skills_.end(),
              [](const SkillInput& a, const SkillInput& b) {
                  if (a.player_id != b.player_id) return a.player_id < b.player_id;
                  return a.seq < b.seq;
              });
}

void Room::submit_move(const MoveInput& in) {
    pending_moves_.push_back(in);   // 只入队，这里绝不能碰 players_ 里的任何字段
}

void Room::submit_skill(const SkillInput& in) {
    pending_skills_.push_back(in);
}

// ---- 移动结算（教程 §5.3）----

void Room::settle_movement() {
    if (pending_moves_.empty()) {
        return;   // 这一帧谁都没动
    }

    for (const MoveInput& in : pending_moves_) {
        const auto it = players_.find(in.player_id);
        if (it == players_.end()) {
            continue;   // 玩家已经离开房间
        }
        Player& p = it->second;
        if (p.state != EntityState::kAlive) {
            continue;   // 死人不能动
        }

        // 幂等去重：回绕安全比较（game/seq.h）。
        // 不能写成 in.seq <= p.last_cmd_seq —— seq 从 65535 回到 0 时
        // 所有指令都会被永久丢弃。
        if (in.seq != 0 && !is_newer_seq(in.seq, p.last_cmd_seq)) {
            ++p.dup_dropped;
            continue;   // 同一条指令或乱序旧包，已处理过
        }
        p.last_cmd_seq = in.seq;

        // 位移：直接加定点量。客户端只传意图，服务端负责限幅 ——
        // 客户端说"我要瞬移 10 万单位"也不能信。
        const std::int32_t dx = std::clamp(in.dx, -kMaxStepFp, kMaxStepFp);
        const std::int32_t dy = std::clamp(in.dy, -kMaxStepFp, kMaxStepFp);

        p.x = std::clamp(p.x + dx, kWorldMin, kWorldMax);
        p.y = std::clamp(p.y + dy, kWorldMin, kWorldMax);

        // 只有真的改了世界状态，才刷新活跃时间。
        // 如果无条件刷新，"被重复包轰炸"的房间永远不会超时回收。
        last_active_frame_ = frame_;
    }
    pending_moves_.clear();
}

// ---- 技能结算（教程 §5.3）----

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
        // ② 幂等：技能指令有自己的序号空间，和移动分开记。
        //    如果共用 last_cmd_seq，移动包一刷新它，后面重发的技能包
        //    就被误判成"新的"，去重直接失效（这个 bug 成因很隐蔽）。
        if (in.seq != 0 && static_cast<std::uint16_t>(in.seq - caster.last_skill_seq) == 0) {
            ++caster.dup_dropped;
            continue;
        }
        if (in.seq != 0) {
            caster.last_skill_seq = in.seq;
        }
        // ③ 技能 CD —— 纯整数帧比较
        if (frame_ < caster.skill_ready_frame) {
            continue;   // 静默丢弃，不回包，防止作弊者据此探测 CD
        }

        const auto tit = players_.find(in.target_id);
        if (tit == players_.end()) {
            continue;   // 目标不存在
        }
        Player& target = tit->second;

        // ④ 不能打自己、不能打队友、目标必须活着
        if (target.id == caster.id) {
            continue;
        }
        if (target.team == caster.team) {
            continue;   // 3v3 没有友伤，这条规则写在服务端才有意义
        }
        if (target.state != EntityState::kAlive) {
            continue;
        }

        // ⑤ 射程校验 —— 反外挂的核心：客户端说"我砍他"，
        //    服务端用自己的权威坐标算距离；超出射程直接不认。
        //    用距离平方比较，不开根号（sqrt 慢几十个周期，且整数平方无精度问题）。
        const std::int64_t d2 = dist_sq_fp(caster.x, caster.y, target.x, target.y);
        const std::int64_t range2 = static_cast<std::int64_t>(kSkillRangeFp) * kSkillRangeFp;
        if (d2 > range2) {
            continue;
        }

        // ⑥ 全部校验通过，才真正结算。
        //    CD 也是"校验通过才扣"：打空不扣 CD 是有意的设计（配合静默丢弃，
        //    作弊者连"我到底能不能打"都探测不出来）。
        caster.skill_ready_frame = frame_ + kSkillCooldownFrames;

        // 伤害按下界裁剪：血不会因为一次超大伤害变成负一大截，
        // 但不会在这里判死 —— 死亡判定统一在第 ④ 阶段做。
        target.hp = std::max<std::int32_t>(0, target.hp - caster.atk);

        last_active_frame_ = frame_;
    }
    pending_skills_.clear();
}

// ---- 死亡与胜负（教程 §5.3）----

void Room::settle_death_and_result() {
    // 统一处理死亡。为什么不写在伤害循环里：伤害循环还会引用 target，
    // 中途删除/标记会让后面的引用失效（经典的迭代器/指针失效来源）。
    for (auto& kv : players_) {
        Player& p = kv.second;
        if (p.state == EntityState::kAlive && p.hp <= 0) {
            p.state = EntityState::kDead;
            p.hp = 0;
            last_active_frame_ = frame_;
        }
    }

    if (state_ != RoomState::kFighting) {
        return;
    }

    // 胜负判定：某一队全灭 → 结算态（留 8 秒给客户端播结算动画）
    bool team_alive[2] = {false, false};
    for (const auto& kv : players_) {
        const Player& p = kv.second;
        if (p.state == EntityState::kAlive && p.team < 2) {
            team_alive[p.team] = true;
        }
    }
    if (!team_alive[0] || !team_alive[1]) {
        state_ = RoomState::kSettled;
        last_active_frame_ = frame_;
    }
}

// ---- AOI（教程 §4.4）----

void Room::update_aoi() {
    // 一帧只构建一次网格，所有人共用。
    // grid_ 是成员，复用同一块内存 —— 如果每帧在栈上 new 一个
    // unordered_map，30Hz × 200 房间就是每秒 6000 次 map 构造析构。
    grid_->clear();
    for (const auto& kv : players_) {
        const Player& p = kv.second;
        if (p.state == EntityState::kAlive) {
            grid_->insert(kv.first, p.x, p.y);
        }
    }

    std::vector<std::uint32_t> current;
    current.reserve(16);

    for (auto& kv : players_) {
        Player& viewer = kv.second;
        const std::uint32_t viewer_id = kv.first;
        if (viewer.state != EntityState::kAlive) {
            continue;
        }

        const std::int64_t cur_cell = cell_key(to_cell(viewer.x), to_cell(viewer.y));

        grid_->query_neighbors(viewer.x, viewer.y, viewer_id, current);
        // query_neighbors 的返回顺序取决于 unordered_map 的遍历顺序，
        // 同一个位置两次查询顺序可能不同 —— 必须排序才能比较和做差。
        std::sort(current.begin(), current.end());

        // 快路径：格没换、视野集合也没变 → 一个包都不用发。
        // 先比 last_cell（两个 int64 比较），比 vector 逐元素比较便宜。
        if (cur_cell == viewer.last_cell && current == viewer.last_neighbors) {
            continue;
        }

        // 慢路径：真正换格或有人进出视野，才算差。
        // 两边都已经是升序，满足 set_difference 的前提。
        std::vector<std::uint32_t> enter;
        std::vector<std::uint32_t> leave;
        std::set_difference(current.begin(), current.end(),
                            viewer.last_neighbors.begin(), viewer.last_neighbors.end(),
                            std::back_inserter(enter));
        std::set_difference(viewer.last_neighbors.begin(), viewer.last_neighbors.end(),
                            current.begin(), current.end(),
                            std::back_inserter(leave));

        // 只给"能看见这个实体"的 viewer 发进入/离开包。
        // 已经在你视野里的实体，客户端本来就有数据，重复发是全然的浪费。
        for (const std::uint32_t id : enter) {
            send_enter_packet(viewer_id, id);
        }
        for (const std::uint32_t id : leave) {
            send_leave_packet(viewer_id, id);
        }

        viewer.last_cell = cur_cell;
        viewer.last_neighbors = current;   // current 下一轮会被 query_neighbors clear
    }
}

/// 进入视野：给 viewer 发这个实体的**全量**状态。
/// 为什么必须全量：这条包同时承担"新实体出现"和"它的基线"两个职责。
/// 只发"3 号进来了"而后面发相对量，客户端没有任何参照，解不出来。
void Room::send_enter_packet(std::uint32_t viewer_id, std::uint32_t entity_id) {
    const auto it = players_.find(entity_id);
    if (it == players_.end()) {
        return;
    }
    const Player& p = it->second;

    DeltaFrame d;
    d.frame = frame_;
    DeltaEntry e;
    e.id = entity_id;
    e.mask = static_cast<std::uint8_t>(kFieldPos | kFieldHp | kFieldFlags);
    e.x = pack_coord(p.x);
    e.y = pack_coord(p.y);
    e.hp_percent = pack_hp_percent(p.hp, p.hp_max);
    e.flags = static_cast<std::uint8_t>((p.state == EntityState::kAlive) ? 1u : 0u);
    d.entries.push_back(e);

    send_(viewer_id, kCmdAoiEnter, encode_delta(d));
}

/// 离开视野：只要一个 id，不需要任何状态。
/// leave 包比 enter 包小得多 —— "只在视野变化时发包"策略的另一半收益：
/// 离开是纯通知（客户端删模型），进入要带完整基线。
void Room::send_leave_packet(std::uint32_t viewer_id, std::uint32_t entity_id) {
    std::vector<std::uint8_t> payload;
    net::write_varint(payload, entity_id);
    send_(viewer_id, kCmdAoiLeave, payload);
}

// ---- 状态同步（教程 §6.4）----

void Room::build_snapshot() {
    snapshot_.frame = frame_;
    snapshot_.entries.clear();
    snapshot_.entries.reserve(players_.size());

    for (const auto& kv : players_) {
        const Player& p = kv.second;
        SnapshotEntry e;
        e.id = kv.first;
        e.x = pack_coord(p.x);
        e.y = pack_coord(p.y);
        e.hp_percent = pack_hp_percent(p.hp, p.hp_max);
        e.flags = static_cast<std::uint8_t>((p.state == EntityState::kAlive) ? 1u : 0u);
        snapshot_.entries.push_back(e);
    }
}

/// 和"这个 viewer 已确认的状态"做差，只保留变化的实体。
/// 注意参数叫 viewer_id：diff 的基准永远是"接收者"的状态，不是服务端的权威状态
/// （教程 §6.4：不同玩家看世界的进度不一样，基线必须每 viewer 一份）。
DeltaFrame Room::diff_for(std::uint32_t viewer_id, bool force_full) {
    DeltaFrame out;
    out.frame = frame_;

    const auto vit = players_.find(viewer_id);
    if (vit == players_.end()) {
        return out;
    }
    const Player& viewer = vit->second;

    // 遍历该 viewer 的视野集合（AOI 已经算好了），而不是所有人。
    for (const std::uint32_t id : viewer.last_neighbors) {
        const auto pit = players_.find(id);
        if (pit == players_.end()) {
            continue;
        }
        const Player& p = pit->second;

        const std::int16_t x = pack_coord(p.x);
        const std::int16_t y = pack_coord(p.y);
        const std::uint8_t hp = pack_hp_percent(p.hp, p.hp_max);
        const std::uint8_t flags = static_cast<std::uint8_t>((p.state == EntityState::kAlive) ? 1u : 0u);

        // 全量：不管变没变都带上，并且 mask 置满（客户端拿它当基线）。
        if (force_full) {
            DeltaEntry e;
            e.id = id;
            e.mask = static_cast<std::uint8_t>(kFieldPos | kFieldHp | kFieldFlags);
            e.x = x; e.y = y; e.hp_percent = hp; e.flags = flags;
            out.entries.push_back(e);
            continue;
        }

        // 增量：跟基线比。
        const auto base_it = viewer.baseline.find(id);
        if (base_it == viewer.baseline.end()) {
            // 没有基线 —— 这个人刚进视野，必须发全量。
            DeltaEntry e;
            e.id = id;
            e.mask = static_cast<std::uint8_t>(kFieldPos | kFieldHp | kFieldFlags);
            e.x = x; e.y = y; e.hp_percent = hp; e.flags = flags;
            out.entries.push_back(e);
            continue;
        }

        const SnapshotEntry& base = base_it->second;
        DeltaEntry e;
        e.id = id;
        if (base.x != x || base.y != y) {
            e.mask = static_cast<std::uint8_t>(e.mask | kFieldPos);
            e.x = x; e.y = y;
        }
        if (base.hp_percent != hp) {
            e.mask = static_cast<std::uint8_t>(e.mask | kFieldHp);
            e.hp_percent = hp;
        }
        if (base.flags != flags) {
            e.mask = static_cast<std::uint8_t>(e.mask | kFieldFlags);
            e.flags = flags;
        }
        if (e.mask != 0) {
            out.entries.push_back(e);
        }
    }
    return out;
}

void Room::broadcast() {
    if (state_ == RoomState::kWaiting) {
        return;   // 还没开打，没什么可同步的
    }

    for (auto& kv : players_) {
        Player& viewer = kv.second;
        const std::uint32_t viewer_id = kv.first;
        if (viewer.state != EntityState::kAlive) {
            continue;
        }

        // 刚进视野的实体靠"baseline 里没有它"自动升级为全量，
        // 所以这里统一按增量算，不需要额外的 force_full 分支。
        const DeltaFrame d = diff_for(viewer_id, false);
        if (d.entries.empty()) {
            continue;   // 什么都没变 —— 一个包都不用发
        }

        const std::vector<std::uint8_t> payload = encode_delta(d);
        send_(viewer_id, kCmdSnapshotDelta, payload);

        // 更新基线：把这一帧发出去的状态记下来，下一帧跟它比。
        for (const DeltaEntry& e : d.entries) {
            SnapshotEntry& base = viewer.baseline[e.id];
            base.id = e.id;
            if ((e.mask & kFieldPos) != 0) { base.x = e.x; base.y = e.y; }
            if ((e.mask & kFieldHp) != 0) { base.hp_percent = e.hp_percent; }
            if ((e.mask & kFieldFlags) != 0) { base.flags = e.flags; }
        }

        // 埋点：人次 = 这条消息里有多少个变化的实体；字节数 = payload 实际长度。
        count_broadcast(viewer_id, d.entries.size(), payload.size());
    }
}

void Room::count_broadcast(std::uint32_t viewer_id, std::size_t entry_count, std::size_t byte_count) {
    (void)viewer_id;
    broadcast_persons_ += entry_count;
    broadcast_bytes_ += byte_count;   // 字节数也要记：人次相等时字节数可能差很多

    // 同时统计"如果完全不做 AOI，全广播会是多少人次"（教程 §4.6）：
    // 每个活着的 viewer 都收到所有活着的实体，减去他自己。
    std::size_t alive_count = 0;
    for (const auto& kv : players_) {
        if (kv.second.state == EntityState::kAlive) {
            ++alive_count;
        }
    }
    for (const auto& kv : players_) {
        if (kv.second.state == EntityState::kAlive) {
            full_broadcast_persons_ += (alive_count > 0 ? alive_count - 1 : 0);
        }
    }
}

// ---- 断线重连（教程 §7.1）----

void Room::on_reconnect(std::uint32_t player_id) {
    const auto it = players_.find(player_id);
    if (it == players_.end()) {
        return;
    }
    Player& p = it->second;
    p.online = true;
    p.offline_since_frame = 0;

    // 客户端本地状态已经没了，服务端这边的基线也要一并清掉 ——
    // 否则 diff_for 会以为"客户端还有上一帧的状态"，只发变化量，
    // 客户端拿到一堆没有基线的相对更新，直接解不出来。
    p.baseline.clear();

    // 视野集合也要清掉：重连后客户端什么都不知道，
    // "所有看得见的人"对它来说都算新进入，必须走全量路径。
    p.last_neighbors.clear();
    p.last_cell = 0;

    // 视野集合要重算。重连不属于任何一帧，但它要修正的恰恰是这个
    // viewer 的视野集合，而视野集合的唯一权威来源就是 update_aoi。
    // 手写一遍九宫格查询等于同一段逻辑维护两份，改一处漏一处，不值。
    update_aoi();

    // 发全量快照：update_aoi 刚把 last_neighbors 填对了，
    // diff_for(force_full=true) 会把看得见的所有实体、所有字段一次性推过去。
    const DeltaFrame full = diff_for(player_id, /*force_full=*/true);
    if (!full.entries.empty()) {
        // 基线要跟着全量一起更新，否则下一帧又会把所有人当成"没基线"再发一次全量。
        for (const DeltaEntry& e : full.entries) {
            SnapshotEntry& base = p.baseline[e.id];
            base.id = e.id;
            base.x = e.x;
            base.y = e.y;
            base.hp_percent = e.hp_percent;
            base.flags = e.flags;
        }
        send_(player_id, kCmdSnapshotFull, encode_delta(full));
    }

    // 顺便把房间的元信息也带上（帧号、状态、存活人数）——客户端需要知道
    // "现在是第几帧"才能开始插值。
    send_room_meta(player_id, frame_);
}

void Room::send_room_meta(std::uint32_t player_id, std::uint64_t frame) {
    std::vector<std::uint8_t> payload;
    net::write_varint(payload, frame);
    net::write_varint(payload, static_cast<std::uint64_t>(state_));
    std::uint64_t alive = 0;
    for (const auto& kv : players_) {
        if (kv.second.state == EntityState::kAlive) {
            ++alive;
        }
    }
    net::write_varint(payload, alive);
    send_(player_id, kCmdRoomMeta, payload);
}

}  // namespace arena::game
