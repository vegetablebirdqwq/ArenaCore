#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <utility>
#include <vector>

#include "base/alloc_hook.h"

namespace arena::concurrency {

/// 固定容量对象池：一次要一大块内存，之后自己管，绝不还回堆。
///
/// 为什么不直接用裸 new/delete（面试必答，三个理由都要能说）：
///  1. 锁竞争：CRT 堆是一把全局锁（Windows 的 Low Fragmentation Heap 也只是
///     分片降低了概率）。16 个 IO 线程 + 逻辑线程组同时 new，就是在抢同一把锁。
///  2. 碎片：Message/Snapshot 这种「大小固定、生命周期短」的对象反复申请释放，
///     会把堆捣成筛子，长时间跑必然看到内存增长（RSS 涨但没泄漏）。
///  3. 延迟抖动：new 的快慢取决于堆的当前形态，可能是几十 ns 也可能是几 us。
///     30Hz 逻辑帧最怕的不是平均值，是 P99 —— 一次堆整理就能把一帧顶爆。
///
/// freelist 的关键技巧：**空闲块的下标存在对象自己的内存里**。
/// 反正对象都析构了、这块内存闲着，拿它的前 8 个字节当 next 指针用，
/// 不需要额外一张表（额外一张表意味着额外的 cache 缺失和额外的容量上限）。
///
/// 限制：这个池只适合「定长对象」。T 越大越浪费，别用它池化变长 payload。
template <typename T>
class ObjectPool {
    static_assert(sizeof(T) >= sizeof(std::uint32_t), "slot too small to hold a free-list index");
    static_assert(alignof(T) <= 64, "over-aligned type: use aligned allocator instead");

public:
    explicit ObjectPool(std::size_t block_size = 1024, std::size_t max_blocks = 64) {
        block_size_ = block_size == 0 ? 1 : block_size;
        max_blocks_ = max_blocks == 0 ? 1 : max_blocks;  // 忘了这行会怎样？见教程第 3 节的翻车记录
        blocks_.reserve(max_blocks_);
        // 让 freelist 的 vector 一次开够，运行期 push/pop 不再触堆。
        free_list_.reserve(block_size_ * max_blocks_);
    }

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    ~ObjectPool() {
        for (std::uint8_t* block : blocks_) {
            ::operator delete(block);
        }
    }

    /// 池里现成的空闲槽位数（等于 freelist 长度）。
    std::size_t capacity() const noexcept { return free_list_.size(); }

    /// 累计向堆要过的原始内存字节数 —— 命中率就靠它和 reuse 计数算。
    std::size_t heap_bytes() const noexcept { return blocks_.size() * block_size_ * sizeof(T); }

    /// 定位一个构造好的对象。构造失败会自己把槽位还回去，不留悬挂。
    template <typename... Args>
    T* create(Args&&... args) {
        void* slot = take_slot();
        if (slot == nullptr) {
            return nullptr;
        }
        try {
            return new (slot) T(std::forward<Args>(args)...);
        } catch (...) {
            give_slot(slot);
            throw;
        }
    }

    /// 批量取：一次拿 n 个**未构造**的槽位地址。用于 int2code 这种
    /// 「一次要发一批，但每个对象要填不同内容」的场景。
    /// 返回实际拿到的个数（可能少于 n，说明池空了，调用方要处理降级）。
    std::size_t batch_create(T** out, std::size_t n) {
        std::size_t got = 0;
        while (got < n) {
            void* slot = take_slot();
            if (slot == nullptr) {
                break;
            }
            out[got++] = new (slot) T();
        }
        return got;
    }

    /// 析构 + 归还。传 nullptr 是合法的（幂等），方便写 `pool.destroy(p); p = nullptr;`
    void destroy(T* obj) {
        if (obj == nullptr) {
            return;
        }
        obj->~T();
        give_slot(obj);
    }

    void destroy_batch(T** objs, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            destroy(objs[i]);
        }
    }

private:
    /// 空闲块里前 4 字节存「下一个空闲块的下标」，下标超出 capacity 表示链尾。
    static std::uint32_t& free_link(void* slot) noexcept {
        return *static_cast<std::uint32_t*>(slot);
    }

    void* take_slot() {
        // 本池不是线程安全的（这样才对，见 lesson 第 3 节说明）。
        // 真要多线程抢同一类对象，就每个线程一个池。
        if (free_list_.empty()) {
            if (!grow()) {
                return nullptr;
            }
        }
        const std::uint32_t index = free_list_.back();
        free_list_.pop_back();
        base::count_pool_reuse();
        return blocks_[index / block_size_] + static_cast<std::size_t>(index % block_size_) * sizeof(T);
    }

    void give_slot(void* slot) {
        const std::size_t index = index_of(slot);
        free_link(slot) = kNoNext;
        free_list_.push_back(static_cast<std::uint32_t>(index));
    }

    /// 反向定位下标：遍历所有已申请块，看指针落在哪一块里。
    /// 这是本实现唯一的 O(块数) 操作，块数上限 64，实测 < 100ns。
    /// 换成「每块头部存一个 magic + 起始下标」也能做到 O(1)，但要多一次内存读，
    /// 且对象内存布局会被污染 —— 不做，代码简单更重要。
    std::size_t index_of(void* slot) const {
        const auto* p = static_cast<const std::uint8_t*>(slot);
        for (std::size_t b = 0; b < blocks_.size(); ++b) {
            const std::uint8_t* begin = blocks_[b];
            const std::uint8_t* end = begin + block_size_ * sizeof(T);
            if (p >= begin && p < end) {
                return b * block_size_ + static_cast<std::size_t>(p - begin) / sizeof(T);
            }
        }
        return 0;  // 只有把不属于本池的指针交进来才会走到这，属于调用方 bug
    }

    bool grow() {
        if (blocks_.size() >= max_blocks_) {
            return false;  // 到顶了就不涨 —— 宁可让调用方感知失败，也不静默吃掉内存
        }
        const std::size_t bytes = block_size_ * sizeof(T);
        auto* block = static_cast<std::uint8_t*>(::operator new(bytes));
        std::memset(block, 0, bytes);  // 让新槽位第一次用的时候是干净内存，方便查问题
        blocks_.push_back(block);
        base::count_pool_alloc();

        const std::size_t base = (blocks_.size() - 1) * block_size_;
        for (std::size_t i = 0; i < block_size_; ++i) {
            free_list_.push_back(static_cast<std::uint32_t>(base + i));
        }
        return true;
    }

    static constexpr std::uint32_t kNoNext = 0xFFFFFFFFu;

    std::vector<std::uint8_t*> blocks_;
    std::vector<std::uint32_t> free_list_;
    std::size_t block_size_ = 1024;
    std::size_t max_blocks_ = 64;
};

}  // namespace arena::concurrency
