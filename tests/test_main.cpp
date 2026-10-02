#include "test_framework.h"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "base/buffer.h"
#include "base/consistent_hash.h"
#include "net/codec.h"

using arena::base::ConsistentHashRing;
using arena::base::RingBuffer;
using arena::net::decode_all;
using arena::net::encode;
using arena::net::kDecodeError;
using arena::net::kHeaderSize;
using arena::net::Packet;

// ------------------------------------------------------------------ RingBuffer

TEST(ring_buffer_append_then_read) {
    RingBuffer rb(64);
    const char* msg = "hello world";
    rb.append(msg, 11);
    CHECK_EQ(rb.readable(), std::size_t{11});
    CHECK_EQ(rb.to_string(), std::string("hello world"));
    rb.consume(11);
    CHECK(rb.empty());
    CHECK_EQ(rb.readable(), std::size_t{0});
}

TEST(ring_buffer_partial_consume_keeps_remainder) {
    RingBuffer rb(64);
    rb.append("abcdef", 6);
    rb.consume(2);
    CHECK_EQ(rb.to_string(), std::string("cdef"));
}

TEST(ring_buffer_compacts_instead_of_growing) {
    RingBuffer rb(16);
    rb.append("0123456789", 10);
    rb.consume(8);  // 存活 2 字节，尾部只剩 6 字节连续空间
    const std::size_t cap_before = rb.capacity();
    rb.append("ABCDEF", 6);  // 需要 6 字节，compact 后刚好够
    CHECK_EQ(rb.capacity(), cap_before);  // 不该扩容
    CHECK_EQ(rb.to_string(), std::string("89ABCDEF"));
}

TEST(ring_buffer_grows_when_compact_is_not_enough) {
    RingBuffer rb(8);
    rb.append("0123456789", 10);  // 必须扩容
    CHECK(rb.capacity() >= 10);
    CHECK_EQ(rb.to_string(), std::string("0123456789"));
}

TEST(ring_buffer_survives_many_wrap_cycles) {
    RingBuffer rb(32);
    for (int i = 0; i < 10000; ++i) {
        rb.append("x", 1);
        CHECK_EQ(rb.readable(), std::size_t{1});
        rb.consume(1);
        CHECK(rb.empty());
    }
    CHECK_EQ(rb.capacity(), std::size_t{32});  // 反复复用，容量不涨
}

// ------------------------------------------------------------------ varint / endian

TEST(varint_round_trip_boundaries) {
    const std::uint64_t values[] = {0,
                                    1,
                                    127,
                                    128,
                                    300,
                                    16383,
                                    16384,
                                    (1ULL << 32) - 1,
                                    std::numeric_limits<std::uint64_t>::max()};
    std::vector<std::uint8_t> buf;
    for (std::uint64_t v : values) {
        arena::net::write_varint(buf, v);
    }
    std::size_t offset = 0;
    for (std::uint64_t expected : values) {
        std::uint64_t got = 0;
        CHECK(arena::net::read_varint(buf.data(), buf.size(), offset, got));
        CHECK_EQ(got, expected);
    }
    CHECK_EQ(offset, buf.size());
}

TEST(varint_uses_one_byte_for_small_values) {
    std::vector<std::uint8_t> buf;
    arena::net::write_varint(buf, 127);
    CHECK_EQ(buf.size(), std::size_t{1});
    buf.clear();
    arena::net::write_varint(buf, 128);
    CHECK_EQ(buf.size(), std::size_t{2});
}

TEST(varint_rejects_truncated_input) {
    std::vector<std::uint8_t> buf;
    arena::net::write_varint(buf, 300);
    std::size_t offset = 0;
    std::uint64_t got = 0;
    // 只给第一个字节（带 continuation 位），应当失败
    CHECK(!arena::net::read_varint(buf.data(), 1, offset, got));
}

TEST(little_endian_helpers) {
    std::uint8_t raw[4] = {0x78, 0x56, 0x34, 0x12};
    CHECK_EQ(arena::net::load_le32(raw), std::uint32_t{0x12345678});
    CHECK_EQ(arena::net::load_le16(raw), std::uint16_t{0x5678});

    std::uint8_t out[4] = {};
    arena::net::store_le32(out, 0xDEADBEEF);
    CHECK_EQ(out[0], std::uint8_t{0xEF});
    CHECK_EQ(out[3], std::uint8_t{0xDE});
}

// ------------------------------------------------------------------ codec

TEST(codec_encode_layout_is_stable) {
    const std::uint8_t payload[3] = {0xAA, 0xBB, 0xCC};
    const auto bytes = encode(0x1234, 0x0007, payload, sizeof(payload));

    CHECK_EQ(bytes.size(), kHeaderSize + 3);
    CHECK_EQ(arena::net::load_le32(bytes.data()), std::uint32_t{7});  // len = 4 + 3
    CHECK_EQ(arena::net::load_le16(bytes.data() + 4), std::uint16_t{0x1234});
    CHECK_EQ(arena::net::load_le16(bytes.data() + 6), std::uint16_t{0x0007});
    CHECK_EQ(bytes[8], std::uint8_t{0xAA});
}

TEST(codec_round_trip) {
    const std::string body = "move:100,200";
    const auto bytes = encode(0x0001, 0x0002, body.data(), body.size());

    RingBuffer rb(1024);
    rb.append(bytes.data(), bytes.size());

    std::vector<Packet> out;
    CHECK_EQ(decode_all(rb, out), std::size_t{1});
    CHECK_EQ(out.size(), std::size_t{1});
    CHECK_EQ(out[0].cmd, std::uint16_t{0x0001});
    CHECK_EQ(out[0].seq, std::uint16_t{0x0002});
    CHECK_EQ(std::string(out[0].payload.begin(), out[0].payload.end()), body);
    CHECK(rb.empty());
}

TEST(codec_decodes_sticky_packets) {
    // 三个包一次性到达（粘包）
    std::vector<std::uint8_t> stream;
    for (std::uint16_t i = 1; i <= 3; ++i) {
        const std::string body = "p" + std::to_string(i);
        const auto bytes = encode(i, i, body.data(), body.size());
        stream.insert(stream.end(), bytes.begin(), bytes.end());
    }

    RingBuffer rb(1024);
    rb.append(stream.data(), stream.size());

    std::vector<Packet> out;
    CHECK_EQ(decode_all(rb, out), std::size_t{3});
    CHECK_EQ(out.size(), std::size_t{3});
    CHECK_EQ(out[2].cmd, std::uint16_t{3});
    CHECK(rb.empty());
}

TEST(codec_handles_split_packets_byte_by_byte) {
    // 一条完整数据流，每次只喂 1 个字节 —— 模拟最恶劣的拆包情形
    std::vector<std::uint8_t> stream;
    for (std::uint16_t i = 1; i <= 5; ++i) {
        const std::string body = "payload-" + std::to_string(i);
        const auto bytes = encode(i, i, body.data(), body.size());
        stream.insert(stream.end(), bytes.begin(), bytes.end());
    }

    RingBuffer rb(1024);
    std::vector<Packet> out;
    for (std::size_t i = 0; i < stream.size(); ++i) {
        rb.append(&stream[i], 1);
        const std::size_t n = decode_all(rb, out);
        CHECK(n != kDecodeError);
    }

    CHECK_EQ(out.size(), std::size_t{5});
    CHECK_EQ(out[0].cmd, std::uint16_t{1});
    CHECK_EQ(out[4].cmd, std::uint16_t{5});
    CHECK_EQ(std::string(out[4].payload.begin(), out[4].payload.end()), std::string("payload-5"));
    CHECK(rb.empty());
}

TEST(codec_keeps_partial_packet_in_buffer) {
    const auto bytes = encode(9, 9, "abc", 3);
    RingBuffer rb(1024);
    rb.append(bytes.data(), bytes.size() - 1);  // 故意少喂最后一个字节

    std::vector<Packet> out;
    CHECK_EQ(decode_all(rb, out), std::size_t{0});
    CHECK_EQ(out.size(), std::size_t{0});
    CHECK_EQ(rb.readable(), bytes.size() - 1);  // 半个包必须原样保留

    rb.append(bytes.data() + bytes.size() - 1, 1);
    CHECK_EQ(decode_all(rb, out), std::size_t{1});
    CHECK(rb.empty());
}

TEST(codec_rejects_oversized_length_field) {
    // 伪造一个超大长度字段，必须被拒绝而不是傻等/爆内存
    std::uint8_t evil[8] = {};
    arena::net::store_le32(evil, 0x7FFFFFFF);  // 2GB
    RingBuffer rb(64);
    rb.append(evil, sizeof(evil));

    std::vector<Packet> out;
    std::string err;
    CHECK_EQ(decode_all(rb, out, &err), kDecodeError);
    CHECK(!err.empty());
}

TEST(codec_rejects_undersized_length_field) {
    std::uint8_t evil[8] = {};
    arena::net::store_le32(evil, 2);  // 小于 cmd+seq，非法
    RingBuffer rb(64);
    rb.append(evil, sizeof(evil));

    std::vector<Packet> out;
    CHECK_EQ(decode_all(rb, out, nullptr), kDecodeError);
}

TEST(codec_accepts_max_size_packet) {
    const std::size_t body = arena::net::kMaxPacketSize - 4;
    std::vector<std::uint8_t> payload(body, 0x5A);
    const auto bytes = encode(1, 1, payload.data(), payload.size());

    RingBuffer rb(body + 64);
    rb.append(bytes.data(), bytes.size());

    std::vector<Packet> out;
    CHECK_EQ(decode_all(rb, out), std::size_t{1});
    CHECK_EQ(out[0].payload.size(), body);
}

// ------------------------------------------------------------------ 一致性哈希

TEST(consistent_hash_expand_moves_less_than_mod) {
    // 教程 §6.1 自己验证 1：4 节点环，1000 个 key，加第 5 个节点后
    // 归属没变的比例 > 75%（取模分片只有 20% 留下，一致性哈希 ≈80%）。
    constexpr std::size_t kKeys = 1000;
    std::vector<std::string> keys;
    keys.reserve(kKeys);
    for (std::size_t i = 0; i < kKeys; ++i) {
        keys.push_back("player:" + std::to_string(i));
    }

    ConsistentHashRing ring4;
    ConsistentHashRing ring5;
    for (int i = 0; i < 4; ++i) {
        ring4.add_node("logic-" + std::to_string(i), 150);
        ring5.add_node("logic-" + std::to_string(i), 150);
    }
    ring5.add_node("logic-4", 150);   // 扩容到 5 台

    std::size_t stayed = 0;
    for (const std::string& k : keys) {
        if (ring4.locate(k) == ring5.locate(k)) {
            ++stayed;
        }
    }
    const double ratio = 100.0 * static_cast<double>(stayed) / static_cast<double>(kKeys);
    std::printf("  扩容后归属未变: %.1f%%\n", ratio);
    CHECK(ratio > 75.0);   // 理想 80%，留采样余量
}

TEST(consistent_hash_remove_node_restores) {
    // 移除节点后，key 重新归到其余节点；同一个节点再加回来，归属与最初一致。
    ConsistentHashRing ring;
    for (int i = 0; i < 4; ++i) {
        ring.add_node("logic-" + std::to_string(i), 150);
    }
    const std::string before = ring.locate("player:42");

    ring.remove_node("logic-2");
    CHECK(ring.node_count() == std::size_t{3});

    ring.add_node("logic-2", 150);
    CHECK(ring.node_count() == std::size_t{4});
    CHECK_EQ(ring.locate("player:42"), before);   // 确定性：哈希可复现
}

TEST(consistent_hash_ring_empty_returns_empty) {
    ConsistentHashRing ring;
    CHECK(ring.empty());
    CHECK(ring.locate("any").empty());   // 空环返回空串，调用方处理
}

// ------------------------------------------------------------------ entry

int main() {
    std::printf("ArenaCore unit tests\n====================\n");
    return test::run_all();
}
