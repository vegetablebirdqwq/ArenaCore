#include "game/room.h"
#include "game/snapshot.h"
#include "game/snapshot_codec.h"

#include <cstddef>

namespace arena::game {

void Room::build_snapshot() {
    // 全量基线帧（每个实体都带完整字段）。
    // 这里生成后并不直接发 —— broadcast 里按玩家做增量 diff。
    // 保留这个函数是为了：断线重连/进房间时能直接拿全量。
    (void)frame_;   // 占位：build_snapshot 的产物由 diff_for 按需生成
}

DeltaFrame Room::diff_for(std::uint32_t viewer_id, bool force_full) {
    DeltaFrame out;
    out.frame = frame_;

    const auto vit = players_.find(viewer_id);
    if (vit == players_.end()) {
        return out;
    }
    const Player& viewer = vit->second;

    for (const std::uint32_t id : viewer.last_neighbors) {
        const auto pit = players_.find(id);
        if (pit == players_.end()) {
            continue;
        }
        const Player& p = pit->second;

        const std::int16_t x = static_cast<std::int16_t>(p.x >> 16);
        const std::int16_t y = static_cast<std::int16_t>(p.y >> 16);
        const std::uint8_t hp = static_cast<std::uint8_t>(
            p.hp_max > 0 ? (p.hp * 100) / p.hp_max : 0);
        const std::uint8_t flags = (p.state == EntityState::kAlive) ? 1u : 0u;

        // 全量：不管变没变都带上，mask 置满（客户端拿它当基线）。
        if (force_full) {
            DeltaEntry e;
            e.id = id;
            e.mask = kFieldPos | kFieldHp | kFieldFlags;
            e.x = x; e.y = y; e.hp_percent = hp; e.flags = flags;
            out.entries.push_back(e);
            continue;
        }

        // 增量：跟基线比。
        const auto base_it = viewer.baseline.find(id);
        if (base_it == viewer.baseline.end()) {
            // 没有基线 —— 刚进视野，必须发全量。
            DeltaEntry e;
            e.id = id;
            e.mask = kFieldPos | kFieldHp | kFieldFlags;
            e.x = x; e.y = y; e.hp_percent = hp; e.flags = flags;
            out.entries.push_back(e);
            continue;
        }

        const SnapshotEntry& base = base_it->second;
        DeltaEntry e;
        e.id = id;
        if (base.x != x || base.y != y) {
            e.mask |= kFieldPos;
            e.x = x; e.y = y;
        }
        if (base.hp_percent != hp) {
            e.mask |= kFieldHp;
            e.hp_percent = hp;
        }
        if (base.flags != flags) {
            e.mask |= kFieldFlags;
            e.flags = flags;
        }
        if (e.mask != 0) {
            out.entries.push_back(e);
        }
    }
    return out;
}

void Room::broadcast() {
    if (state_ != RoomState::kFighting) {
        return;                    // 还没开打（或已结束），没什么可同步的
    }

    for (auto& [viewer_id, viewer] : players_) {
        if (viewer.state != EntityState::kAlive) {
            continue;
        }

        const DeltaFrame d = diff_for(viewer_id, false);
        if (d.entries.empty()) {
            continue;              // 什么都没变 —— 一个包都不用发
        }

        const std::vector<std::uint8_t> payload = encode_delta(d);
        if (send_) {
            send_(viewer_id, kCmdSnapshotDelta, payload);
        }

        // 更新基线：把这一帧发出去的状态记下来，下一帧跟它比。
        for (const DeltaEntry& e : d.entries) {
            SnapshotEntry& base = viewer.baseline[e.id];
            base.id = e.id;
            if ((e.mask & kFieldPos) != 0) { base.x = e.x; base.y = e.y; }
            if ((e.mask & kFieldHp) != 0) { base.hp_percent = e.hp_percent; }
            if ((e.mask & kFieldFlags) != 0) { base.flags = e.flags; }
        }

        broadcast_count_ += d.entries.size();
        broadcast_bytes_ += payload.size();
    }
}

void Room::on_reconnect(std::uint32_t player_id) {
    const auto it = players_.find(player_id);
    if (it == players_.end()) {
        return;
    }
    Player& p = it->second;
    p.online = true;
    p.offline_since_frame = 0;

    // 客户端本地状态没了，服务端基线也要清掉 —— 否则 diff_for 只发变化量，
    // 客户端拿到一堆没有基线的相对更新，解不出来。
    p.baseline.clear();

    // 视野集合清掉：重连后一切都要走全量路径。
    p.last_neighbors.clear();
    p.last_cell = 0;

    // 重算视野集合（唯一权威来源是 update_aoi）。
    update_aoi();

    // 发全量快照。
    const DeltaFrame full = diff_for(player_id, /*force_full=*/true);
    if (!full.entries.empty()) {
        const std::vector<std::uint8_t> payload = encode_delta(full);
        if (send_) {
            send_(player_id, kCmdSnapshotFull, payload);
        }

        // 基线跟着全量一起更新，否则下一帧又发一次全量。
        for (const DeltaEntry& e : full.entries) {
            SnapshotEntry& base = p.baseline[e.id];
            base.id = e.id;
            if ((e.mask & kFieldPos) != 0) { base.x = e.x; base.y = e.y; }
            if ((e.mask & kFieldHp) != 0) { base.hp_percent = e.hp_percent; }
            if ((e.mask & kFieldFlags) != 0) { base.flags = e.flags; }
        }
    }
}

}  // namespace arena::game
