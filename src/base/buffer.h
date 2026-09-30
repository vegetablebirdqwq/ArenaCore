#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace arena::base {

/// 环形缓冲（read/write cursor 型，不做真正的“环回写”，而是靠 compact + 扩容）。
///
/// 设计取舍：
///  - 读写游标只前进：`consume()` 推进 read_cursor，`commit_write()` 推进 write_cursor。
///  - 尾部连续可写空间不足时，**先 compact**（把存活数据 memmove 到开头），
///    空间仍不够才**扩容**（倍增）。
///  - 因为 TCP 收包是「读一段 → 解包 → 消费掉」的模式，正常路径下几乎不会触发 compact，
///    更不会触发扩容 —— 这条路径上零内存分配、零数据搬移。
///
/// 面试可讲：为什么不用 std::vector + erase？因为 erase 是 O(n) 搬移；
/// 为什么不像 muduo 那样做「双段连续空间」？因为要处理环形回绕，解包代码复杂度翻倍，
/// 而我们实测 compact 的触发频率极低，不值得用可读性去换。
class RingBuffer {
public:
    explicit RingBuffer(std::size_t initial_capacity = 8192)
        : buf_(initial_capacity == 0 ? 1024 : initial_capacity) {}

    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    const std::uint8_t* read_ptr() const noexcept { return buf_.data() + read_pos_; }
    std::uint8_t* write_ptr() noexcept { return buf_.data() + write_pos_; }

    std::size_t readable() const noexcept { return write_pos_ - read_pos_; }
    std::size_t writable() const noexcept { return buf_.size() - write_pos_; }
    std::size_t capacity() const noexcept { return buf_.size(); }
    bool empty() const noexcept { return read_pos_ == write_pos_; }

    /// 保证从 write_ptr() 起至少有 n 字节**连续**可写空间。
    void ensure_writable(std::size_t n) {
        if (writable() >= n) {
            return;
        }
        const std::size_t live = readable();
        if (capacity() >= live + n) {
            compact();
            return;
        }
        std::size_t next = capacity();
        while (next < live + n) {
            next *= 2;
        }
        std::vector<std::uint8_t> bigger(next);
        if (live != 0) {
            std::memcpy(bigger.data(), read_ptr(), live);
        }
        buf_.swap(bigger);
        read_pos_ = 0;
        write_pos_ = live;
    }

    void commit_write(std::size_t n) noexcept { write_pos_ += n; }

    void consume(std::size_t n) noexcept {
        const std::size_t live = readable();
        read_pos_ += (n > live ? live : n);
        if (read_pos_ == write_pos_) {
            read_pos_ = 0;
            write_pos_ = 0;  // 全部消费完就复位，避免游标无限增长
        }
    }

    /// 拷贝式追加（便捷路径）。
    void append(const void* src, std::size_t n) {
        ensure_writable(n);
        if (n != 0) {
            std::memcpy(write_ptr(), src, n);
        }
        commit_write(n);
    }

    /// 把存活数据搬到缓冲区开头，使尾部连续空间最大化。
    void compact() noexcept {
        const std::size_t live = readable();
        if (live != 0 && read_pos_ != 0) {
            std::memmove(buf_.data(), buf_.data() + read_pos_, live);
        }
        read_pos_ = 0;
        write_pos_ = live;
    }

    std::string to_string() const {
        return std::string(reinterpret_cast<const char*>(read_ptr()), readable());
    }

private:
    std::vector<std::uint8_t> buf_;
    std::size_t read_pos_ = 0;
    std::size_t write_pos_ = 0;
};

}  // namespace arena::base
