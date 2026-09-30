#include "net/codec.h"

#include <cstring>

namespace arena::net {

namespace {
constexpr std::uint64_t kVarintContinuation = 0x80ULL;
constexpr std::uint64_t kVarintPayloadMask = 0x7FULL;
constexpr int kVarintMaxShifts = 10;  // 10 * 7 = 70 >= 64
}  // namespace

std::uint16_t load_le16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[0]) |
                                      (static_cast<std::uint16_t>(p[1]) << 8));
}

std::uint32_t load_le32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

void store_le16(std::uint8_t* p, std::uint16_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v & 0xFF);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
}

void store_le32(std::uint8_t* p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v & 0xFF);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
}

void write_varint(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (;;) {
        const std::uint8_t byte = static_cast<std::uint8_t>(value & kVarintPayloadMask);
        value >>= 7;
        if (value == 0) {
            out.push_back(byte);
            return;
        }
        out.push_back(static_cast<std::uint8_t>(byte | kVarintContinuation));
    }
}

bool read_varint(const std::uint8_t* data, std::size_t size, std::size_t& offset, std::uint64_t& value) {
    std::uint64_t result = 0;
    int shift = 0;
    std::size_t pos = offset;
    while (pos < size && shift < kVarintMaxShifts * 7) {
        const std::uint8_t byte = data[pos++];
        result |= (static_cast<std::uint64_t>(byte & kVarintPayloadMask) << shift);
        if ((byte & kVarintContinuation) == 0) {
            offset = pos;
            value = result;
            return true;
        }
        shift += 7;
    }
    return false;  // 半截 varint 或超长编码，两种都当非法
}

std::vector<std::uint8_t> encode(std::uint16_t cmd,
                                 std::uint16_t seq,
                                 const void* payload,
                                 std::size_t len) {
    std::vector<std::uint8_t> out(kHeaderSize + len);
    store_le32(out.data(), static_cast<std::uint32_t>(kMinLength + len));
    store_le16(out.data() + 4, cmd);
    store_le16(out.data() + 6, seq);
    if (len != 0) {
        std::memcpy(out.data() + kHeaderSize, payload, len);
    }
    return out;
}

std::size_t decode_all(base::RingBuffer& in, std::vector<Packet>& out, std::string* error) {
    std::size_t decoded = 0;
    for (;;) {
        if (in.readable() < kLengthFieldSize) {
            return decoded;  // 连长度字段都没凑齐
        }

        const std::uint8_t* head = in.read_ptr();
        const std::uint32_t len = load_le32(head);

        if (len < kMinLength || len > kMaxPacketSize) {
            if (error != nullptr) {
                *error = "illegal packet length: " + std::to_string(len);
            }
            return kDecodeError;
        }
        if (in.readable() < kLengthFieldSize + len) {
            return decoded;  // 半包，等下一次 recv
        }

        const std::uint8_t* body = head + kLengthFieldSize;
        Packet pkt;
        pkt.cmd = load_le16(body);
        pkt.seq = load_le16(body + 2);
        pkt.payload.assign(body + 4, body + len);

        out.push_back(std::move(pkt));
        in.consume(kLengthFieldSize + len);
        ++decoded;
    }
}

}  // namespace arena::net
