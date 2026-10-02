#pragma once

#include <cstdint>
#include <vector>

namespace arena::game {

/// 增量字段的位掩码。用位掩码而不是"每个字段带一个 present 标志"，
/// 因为 6 个字段用标志要 6 字节，用掩码只要 1 字节。
enum DeltaField : std::uint8_t {
    kFieldPos    = 1u << 0,
    kFieldHp     = 1u << 1,
    kFieldFacing = 1u << 2,
    kFieldFlags  = 1u << 3,
};

/// 一帧里某个实体的变化。字段窄得离谱是故意的 —— 广播是 N 倍放大。
struct DeltaEntry {
    std::uint32_t id = 0;
    std::uint8_t  mask = 0;
    std::int16_t  x = 0;          // 世界单位，仅 mask & kFieldPos 时有效
    std::int16_t  y = 0;
    std::uint8_t  hp_percent = 0; // 0..100，仅 mask & kFieldHp 时有效
    std::uint8_t  flags = 0;
};

struct DeltaFrame {
    std::uint64_t frame = 0;
    std::vector<DeltaEntry> entries;
};

}  // namespace arena::game
