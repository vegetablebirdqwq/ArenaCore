#pragma once

#include <cstdint>

namespace arena::game {

/// 回绕安全的序号比较：seq 是 uint16，从 65535 回到 0 时不能出错。
/// 语义：a 是否比 b 更新（在回绕窗口内，教程 §7.2）。
inline bool is_newer_seq(std::uint16_t a, std::uint16_t b) noexcept {
    // 把差算成 uint16 再看落在窗口的哪一半：半个序号空间以内算"更新"。
    // 必须排除 diff == 0（完全相同的序号 = 重复包重放）：否则幂等去重会失效，
    // 同一条指令被当成"更新"处理两次。这是教程 §7.3 回绕测试抓的那个 bug。
    const std::uint16_t diff = static_cast<std::uint16_t>(a - b);
    return diff != 0 && diff < 0x8000u;
}

}  // namespace arena::game
