#pragma once

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>

#include "game/world.h"

namespace arena::game {

inline constexpr std::int32_t kCellSize = 128;
inline constexpr std::int32_t kCellSizeFp = kCellSize * kFpOne;

/// 格坐标 → 64 位单整数 key。
///
/// 布局：(cx << 32) | (cy & 0xFFFFFFFF)
///  - cx 可能在 [-64, 64] 附近（±8000 单位 ÷ 128），高位有大量余量。
///  - 低 32 位用掩码取，负的 cy 转成无符号后是补码，二进制模式唯一，不冲突。
///
/// 为什么不用 cx * 100000 + cy：乘积会溢出、正负号会互相污染，
/// 而且乘法的中间结果如果接近 INT64_MAX 就是 UB。位运算是零代价且无歧义的。
inline std::int64_t cell_key(std::int32_t cx, std::int32_t cy) noexcept {
    return (static_cast<std::int64_t>(cx) << 32) |
           static_cast<std::int64_t>(static_cast<std::uint32_t>(cy));
}

/// 世界坐标（定点）→ 格坐标。
/// 注意负数除法的方向：C++ 整数除法是向零取整，-1/128 == 0，
/// 而我们需要 -1 落在 cx = -1 这一格里。所以负数要单独处理。
inline std::int32_t to_cell(std::int32_t fp_coord) noexcept {
    // std::floor 语义的定点版。
    // 注意：不能用「加 (b-1) 再除」的写法 —— 那对任意负数都不等于 floor，
    // 例：(-1 + (b-1))/b = 0，而 floor(-1/b) = -1（教程 §4.3 的坑，实测抓到的）。
    // 用「正负分支」：负数时按 -((-a + b - 1)/b) 算。
    if (fp_coord >= 0) {
        return fp_coord / kCellSizeFp;
    }
    return -((-(static_cast<std::int64_t>(fp_coord)) + kCellSizeFp - 1) / kCellSizeFp);
}

class AoiGrid {
public:
    /// 清空。Room 每帧重建网格，靠这个复用内存。
    void clear();

    /// 把实体放进网格。同一个 id 重复加入会先移除旧的。
    void insert(std::uint32_t id, std::int32_t x_fp, std::int32_t y_fp);
    void erase(std::uint32_t id);

    /// 查询某个位置周围九宫格里的所有实体 id（不含 exclude_id 自己）。
    void query_neighbors(std::int32_t x_fp,
                         std::int32_t y_fp,
                         std::uint32_t exclude_id,
                         std::vector<std::uint32_t>& out) const;

    std::int32_t cell_of(std::int32_t fp_coord) const noexcept { return to_cell(fp_coord); }

private:
    struct Cell {
        std::vector<std::uint32_t> ids;
    };
    std::unordered_map<std::int64_t, Cell> cells_;
    std::unordered_map<std::uint32_t, std::int64_t> where_;  // id → 所在格 key
};

}  // namespace arena::game
