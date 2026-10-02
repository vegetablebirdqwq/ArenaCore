#include "game/room_manager.h"

#include <utility>

namespace arena::game {

namespace {
// 房间最多 6 人（3v3）。玩家数上限也用它。
inline constexpr std::size_t kRoomCapacity = 6;
// 空闲房间多久（帧）后回收。30Hz 下 10 秒 = 300 帧。
inline constexpr std::uint64_t kReapAfterFrames = 300;
}  // namespace

std::uint32_t RoomManager::join_or_create(std::uint32_t player_id, std::uint8_t team,
                                          const std::string& name) {
    // 已在某个房间：直接返回（幂等，重连场景）
    const auto existing = player_room_.find(player_id);
    if (existing != player_room_.end()) {
        return existing->second;
    }

    // 找一个有空位且还在等待的房间（不满 6 人，未开打）
    for (auto& [room_id, room] : rooms_) {
        if (room->state() == RoomState::kWaiting && room->player_count() < kRoomCapacity) {
            room->add_player(player_id, team, name);
            player_room_[player_id] = room_id;
            return room_id;
        }
    }

    // 没有合适的房间，新建一个
    const std::uint32_t room_id = next_room_id_++;
    auto room = std::make_unique<Room>(room_id, send_);   // send_ 可能为空，Room 内部已判空
    room->add_player(player_id, team, name);
    player_room_[player_id] = room_id;
    rooms_.emplace(room_id, std::move(room));
    return room_id;
}

Room* RoomManager::find_room_of_player(std::uint32_t player_id) {
    const auto it = player_room_.find(player_id);
    if (it == player_room_.end()) {
        return nullptr;
    }
    return find_room(it->second);
}

Room* RoomManager::find_room(std::uint32_t room_id) {
    const auto it = rooms_.find(room_id);
    return it == rooms_.end() ? nullptr : it->second.get();
}

void RoomManager::tick_all(Clock::time_point now) {
    for (auto& [room_id, room] : rooms_) {
        room->tick(now);
    }
}

std::size_t RoomManager::reap_idle(std::uint64_t current_frame) {
    std::size_t reaped = 0;
    std::vector<std::uint32_t> to_erase;
    for (auto& [room_id, room] : rooms_) {
        const bool empty = room->player_count() == 0;
        const bool settled_idle =
            room->state() == RoomState::kSettled &&
            current_frame - room->last_active_frame() > kReapAfterFrames;
        if (empty || settled_idle) {
            to_erase.push_back(room_id);
        }
    }
    for (const std::uint32_t rid : to_erase) {
        // 把该房间里所有玩家的反查表清掉
        Room* room = find_room(rid);
        if (room != nullptr) {
            for (const auto& [pid, _] : room->players()) {
                player_room_.erase(pid);
            }
        }
        rooms_.erase(rid);
        ++reaped;
    }
    return reaped;
}

}  // namespace arena::game
