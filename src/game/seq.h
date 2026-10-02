#pragma once

#include <cstdint>

namespace arena::game {

/// 回绕安全的序号比较：seq 是 uint16，从 65535 回到 0 时不能出错。
/// 语义：a 是否比 b 更新（在回绕窗口内）。
inline bool is_newer_seq(std::uint16_t a, std::uint16_t b) noexcept {
    return static_cast<std::uint16_t>(a - b) < 0x8000u;
}

}  // namespace arena::game
