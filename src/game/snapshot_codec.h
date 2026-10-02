#pragma once

#include <cstdint>
#include <vector>

#include "game/snapshot.h"

namespace arena::game {

/// 把定点坐标压成 int16 世界单位。内联共享，编码/解码两侧用同一份（避免改歪）。
inline std::int16_t pack_coord(std::int32_t fp) noexcept {
    // int32 的 >> 是算术右移（MSVC/GCC/Clang 全部如此），负数保留符号位。
    return static_cast<std::int16_t>(fp >> 16);
}

inline std::int32_t unpack_coord(std::int16_t v) noexcept {
    return static_cast<std::int32_t>(v) << 16;
}

inline std::uint8_t pack_hp_percent(std::int32_t hp, std::int32_t hp_max) noexcept {
    return hp_max > 0 ? static_cast<std::uint8_t>((hp * 100) / hp_max) : 0;
}

/// 把 DeltaFrame 编码成 payload。调用方负责再套一层 net::encode(cmd, seq, payload)。
std::vector<std::uint8_t> encode_delta(const DeltaFrame& d);

/// 解码。仅测试和 Bot 用；服务端自己不需要解码自己发的包。
/// 返回 false 表示 payload 被截断或含非法 varint。
bool decode_delta(const std::uint8_t* data, std::size_t size, DeltaFrame& out);

}  // namespace arena::game
