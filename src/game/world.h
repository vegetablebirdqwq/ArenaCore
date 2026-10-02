#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace arena::game {

// 固定点坐标：Q16.16，1.0 == 65536。
// 为什么不用 float：战斗结算要确定，浮点没有跨平台/跨优化级别的一致性。
inline constexpr std::int32_t kFpOne = 1 << 16;

// 世界边界（定点）。±8000 单位，够 3v3 的小地图用。
inline constexpr std::int32_t kWorldMin = -8000 * kFpOne;
inline constexpr std::int32_t kWorldMax = 8000 * kFpOne;

inline float fp_to_float(std::int32_t v) noexcept {
    return static_cast<float>(v) / static_cast<float>(kFpOne);
}

inline std::int32_t float_to_fp(float v) noexcept {
    // lround 而不是隐式截断：截断会让 -0.9 变成 0，正负不对称，
    // 而这种不对称在"同一帧两个人对称移动"的场景下会暴露成位置不等。
    return static_cast<std::int32_t>(std::lround(static_cast<double>(v) * kFpOne));
}

enum class EntityState : std::uint8_t {
    kAlive = 0,
    kDead = 1,
};

/// 快照里的一个实体条目。注意字段全是窄类型：
/// 广播是 N 倍放大的，这里省下的每一个字节都要乘 N。
struct SnapshotEntry {
    std::uint32_t id = 0;
    std::int16_t  x = 0;           // 世界单位，int16 够 ±32767
    std::int16_t  y = 0;
    std::uint8_t  hp_percent = 0;  // 0..100
    std::uint8_t  flags = 0;       // bit0 = alive
};

struct Player {
    std::uint32_t id = 0;
    std::uint8_t  team = 0;              // 0 或 1，3v3
    std::string   name;

    std::int32_t x = 0;                  // 定点
    std::int32_t y = 0;

    // hp 用千分比整数（见第 6 节）：1000 == 满血。
    // 为什么不是 float：血量比较是判定边界，浮点会飘。
    std::int32_t hp = 1000;
    std::int32_t hp_max = 1000;
    std::int32_t atk = 120;              // 一刀掉 120 千分比 = 12%

    EntityState state = EntityState::kAlive;

    // 技能 CD 用"帧到期时刻"，不用浮点秒数。
    // 帧计数是整数，永远精确；浮点倒计时在 30Hz 下会漂。
    std::uint64_t skill_ready_frame = 0;

    // 幂等去重：客户端指令序号单调递增。
    // 移动和技能各有一个独立的序号空间 —— 它们走不同的命令号，
    // 客户端各发各的，混在一起记会让去重失效。
    std::uint16_t last_cmd_seq = 0;
    std::uint16_t last_skill_seq = 0;

    bool online = true;
    std::uint64_t offline_since_frame = 0;
    std::uint32_t dup_dropped = 0;       // 被去重丢掉的包数（埋点用）

    std::int64_t last_cell = 0;          // 上一帧所在格 key，用于"没换格"快路径
};

}  // namespace arena::game
