#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "base/buffer.h"

namespace arena::net {

/// 线上包格式（全部小端）：
///
///   +--------+--------+--------+-------------------+
///   | len:4  | cmd:2  | seq:2  | payload (len-4)   |
///   +--------+--------+--------+-------------------+
///
///  - `len` **不含自身这 4 字节**，等于 4 + payload_size。
///  - `cmd` 命令号，决定 payload 如何解释。
///  - `seq` 用于请求/响应配对与去重（uint16 回绕即视为旧包）。
///  - 合法范围：kMinLength <= len <= kMaxPacketSize，越界即判定为恶意/非法包，直接断连。
///
/// 为什么不用 JSON：同一条移动指令 JSON 要 80+ 字节，二进制 16 字节搞定；
/// 而且 JSON 解析要建 DOM、要堆分配，30Hz × 上万连接的路径上这是致命的。
/// 为什么不用 Protobuf：不是不能用，而是本项目零依赖；
/// 而且面试官更想看你能不能讲清「长度前缀 + 变长整数」这几个字节是怎么排的。
inline constexpr std::uint32_t kMaxPacketSize = 64u * 1024u;
inline constexpr std::uint32_t kMinLength = 4u;  // cmd(2) + seq(2)
inline constexpr std::size_t kLengthFieldSize = 4;
inline constexpr std::size_t kHeaderSize = 8;  // len(4) + cmd(2) + seq(2)

struct Packet {
    std::uint16_t cmd = 0;
    std::uint16_t seq = 0;
    std::vector<std::uint8_t> payload;
};

/// 编码一个完整的包（含 4 字节长度前缀）。
std::vector<std::uint8_t> encode(std::uint16_t cmd,
                                 std::uint16_t seq,
                                 const void* payload,
                                 std::size_t len);

inline std::vector<std::uint8_t> encode(std::uint16_t cmd, std::uint16_t seq) {
    return encode(cmd, seq, nullptr, 0);
}

/// 解包失败（长度字段非法 / 超过上限）时的返回值。
inline constexpr std::size_t kDecodeError = static_cast<std::size_t>(-1);

/// 从 in 中反复解包，直到不足一个完整包为止（半包原样留在 in 里等下次）。
/// 返回解出的完整包个数，结果追加到 out；遇到非法包返回 kDecodeError。
std::size_t decode_all(base::RingBuffer& in, std::vector<Packet>& out, std::string* error = nullptr);

// ---------------------------------------------------------------- LEB128 变长整数
// 小数值占 1 字节，大数值才变长。用于 payload 内部的整数编码（坐标、ID、长度）。
void write_varint(std::vector<std::uint8_t>& out, std::uint64_t value);
bool read_varint(const std::uint8_t* data, std::size_t size, std::size_t& offset, std::uint64_t& value);

// ---------------------------------------------------------------- 小端读写
std::uint16_t load_le16(const std::uint8_t* p) noexcept;
std::uint32_t load_le32(const std::uint8_t* p) noexcept;
void store_le16(std::uint8_t* p, std::uint16_t v) noexcept;
void store_le32(std::uint8_t* p, std::uint32_t v) noexcept;

}  // namespace arena::net
