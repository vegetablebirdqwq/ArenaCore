#include "game/snapshot_codec.h"

#include "net/codec.h"

namespace arena::game {

/// 编码一个增量帧。
/// payload 布局（整数部分全部小端）：
///   frame:varint | count:varint | [ id:varint | mask:1 | 按 mask 出现的字段 ]
std::vector<std::uint8_t> encode_delta(const DeltaFrame& d) {
    std::vector<std::uint8_t> out;
    out.reserve(16 + d.entries.size() * 8);

    net::write_varint(out, d.frame);
    net::write_varint(out, static_cast<std::uint64_t>(d.entries.size()));

    for (const DeltaEntry& e : d.entries) {
        net::write_varint(out, e.id);
        // mask 直接 1 字节。这里不用 varint：mask 常是 1/2/3，
        // varint 也是 1 字节，但多一次循环判断，不值得。
        out.push_back(e.mask);

        if ((e.mask & kFieldPos) != 0) {
            std::uint8_t tmp[4];
            net::store_le16(tmp, static_cast<std::uint16_t>(e.x));
            net::store_le16(tmp + 2, static_cast<std::uint16_t>(e.y));
            out.insert(out.end(), tmp, tmp + 4);
        }
        if ((e.mask & kFieldHp) != 0) {
            out.push_back(e.hp_percent);
        }
        if ((e.mask & kFieldFlags) != 0) {
            out.push_back(e.flags);
        }
    }
    return out;
}

bool decode_delta(const std::uint8_t* data, std::size_t size, DeltaFrame& out) {
    std::size_t off = 0;
    std::uint64_t frame = 0;
    if (!net::read_varint(data, size, off, frame)) {
        return false;
    }
    std::uint64_t count = 0;
    if (!net::read_varint(data, size, off, count)) {
        return false;
    }

    out.frame = frame;
    out.entries.clear();

    for (std::uint64_t i = 0; i < count; ++i) {
        std::uint64_t id = 0;
        if (!net::read_varint(data, size, off, id)) {
            return false;
        }
        if (off >= size) {
            return false;
        }
        const std::uint8_t mask = data[off++];

        DeltaEntry e;
        e.id = static_cast<std::uint32_t>(id);
        e.mask = mask;
        if ((mask & kFieldPos) != 0) {
            if (off + 4 > size) {
                return false;
            }
            e.x = static_cast<std::int16_t>(net::load_le16(data + off));
            e.y = static_cast<std::int16_t>(net::load_le16(data + off + 2));
            off += 4;
        }
        if ((mask & kFieldHp) != 0) {
            if (off >= size) {
                return false;
            }
            e.hp_percent = data[off++];
        }
        if ((mask & kFieldFlags) != 0) {
            if (off >= size) {
                return false;
            }
            e.flags = data[off++];
        }
        out.entries.push_back(e);
    }
    return off == size;   // 多出来的字节说明编码/解码不一致，明确报错比静默忽略好
}

}  // namespace arena::game
