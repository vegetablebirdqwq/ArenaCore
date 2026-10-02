#pragma once

#include <cstdint>
#include <vector>

#include "net/codec.h"

/// 协议常量与 payload 定义。**压测 Bot 和服务器必须共用这一个头文件。**
///
/// 为什么要单拎出来说这件事：如果 Bot 自己复制一份命令号常量，
/// 某天服务器把 kMoveReq 从 3 改成 0x0103，压测照样能跑、照样"0 错误"，
/// 但测的已经不是真实协议路径了 —— 这种假 green 比压测跑不起来更可怕。
/// 所以规则是：Bot 只链接服务器的协议库，常量一律 include 过来。
namespace arena::net {

// ---- 客户端 → 服务器（请求）----
inline constexpr std::uint16_t kMoveReq = 0x0003;   // 移动意图（dx, dy 定点）
inline constexpr std::uint16_t kSkillReq = 0x0004;  // 技能意图（skill_id, target_id）

// ---- 服务器 → 客户端（推送/应答）----
inline constexpr std::uint16_t kSnapshotFull = 0x0301;   // 全量快照（进视野/重连）
inline constexpr std::uint16_t kSnapshotDelta = 0x0302;  // 增量快照（每帧广播）
inline constexpr std::uint16_t kRoomMeta = 0x0305;       // 房间元信息

/// 移动 payload：dx, dy（定点 Q16.16，zigzag 压成 varint）。
inline std::vector<std::uint8_t> make_move_payload(std::int32_t dx, std::int32_t dy) {
    std::vector<std::uint8_t> out;
    const auto zig = [](std::int32_t v) -> std::uint64_t {
        return static_cast<std::uint64_t>((static_cast<std::uint32_t>(v) << 1) ^
                                          static_cast<std::uint32_t>(v >> 31));
    };
    write_varint(out, zig(dx));
    write_varint(out, zig(dy));
    return out;
}

/// 技能 payload：skill_id + target_id（varint）。
inline std::vector<std::uint8_t> make_skill_payload(std::uint16_t skill_id,
                                                    std::uint32_t target_id) {
    std::vector<std::uint8_t> out;
    write_varint(out, skill_id);
    write_varint(out, target_id);
    return out;
}

/// 从 payload 里读第 n 个 varint。读不出来（半截/越界）返回 false。
inline bool read_varint_at(const std::vector<std::uint8_t>& payload,
                           std::size_t index,
                           std::uint64_t& value) {
    std::size_t offset = 0;
    for (std::size_t i = 0; i <= index; ++i) {
        if (!read_varint(payload.data(), payload.size(), offset, value)) {
            return false;
        }
    }
    return true;
}

}  // namespace arena::net
