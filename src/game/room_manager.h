#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "game/room.h"

namespace arena::game {

/// 房间管理器：多房间 + 按玩家分配 + 空闲回收。
///
/// 它解决什么问题：压测发现单房间广播是 O(N²) —— 所有连接塞进一个房间，
/// 每帧要给每个玩家广播快照 = N×N 的组合，500 连接直接崩溃（见 docs/压测报告.md）。
/// 真实系统必须分房间：每个房间最多 6 人（3v3），广播只发生在房间内部，
/// 连接数再多也只影响房间数量，不放大单房间的广播成本。
///
/// 线程模型：RoomManager 和 Room 都在逻辑线程里跑（单线程），不需要锁。
/// 网络线程把输入 submit 进来时通过队列转交（本实现里由调用方保证串行）。
class RoomManager {
public:
    /// 设置新建房间用的发送回调（由逻辑层/发送层提供）。创建房间时传给 Room。
    void set_send_fn(SendFn fn) { send_ = std::move(fn); }

    /// 找有空位的房间（未满 6 人且未开打），没有就新建一个。
    /// 返回房间 id。玩家加入后由调用方把连接绑定到该房间。
    std::uint32_t join_or_create(std::uint32_t player_id, std::uint8_t team,
                                 const std::string& name);

    /// 按玩家 id 查所在房间。不在任何房间返回 nullptr。
    Room* find_room_of_player(std::uint32_t player_id);

    /// 按房间 id 查。
    Room* find_room(std::uint32_t room_id);

    /// 推进所有房间的逻辑帧（逻辑线程每 tick 调一次）。
    void tick_all(Clock::time_point now);

    /// 回收已结束（kSettled 且超时）或空房间。返回回收数量。
    std::size_t reap_idle(std::uint64_t current_frame);

    std::size_t room_count() const noexcept { return rooms_.size(); }

    /// 房间 id → 房间（只读，给压测/验证用）。
    const std::unordered_map<std::uint32_t, std::unique_ptr<Room>>& rooms() const noexcept {
        return rooms_;
    }

private:
    std::uint32_t next_room_id_ = 1;
    std::unordered_map<std::uint32_t, std::unique_ptr<Room>> rooms_;
    // 玩家 → 房间 id（快速反查）
    std::unordered_map<std::uint32_t, std::uint32_t> player_room_;
    SendFn send_;   // 新建房间用的发送回调
};

}  // namespace arena::game
