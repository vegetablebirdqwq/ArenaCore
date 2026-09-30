#pragma once

#include <atomic>
#include <cstddef>

// 堆分配计数 hook。
// 用途只有一个：量化「优化前后堆分配次数」，也就是方案书里那条
// 「堆分配次数优化后下降 >= 80%」的数字到底怎么来的。
//
// 三个计数器是分开的，别混：
//   g_new_count    —— 全局 new 的次数（进程内所有堆分配）
//   g_pool_alloc   —— 对象池「必须向内层要内存」的次数（只有扩容时才涨）
//   g_pool_reuse   —— 对象池「从 freelist 复用」的次数（这才是省下来的）
//
// 面试时那张对比表的分子分母就来自这里：优化前 g_new_count 飞涨，
// 优化后 g_pool_reuse 飞涨而 g_pool_alloc 基本不动。
namespace arena::base {

inline std::atomic<unsigned long long> g_new_count{0};
inline std::atomic<unsigned long long> g_pool_alloc{0};
inline std::atomic<unsigned long long> g_pool_reuse{0};

inline void count_new() noexcept {
    g_new_count.fetch_add(1, std::memory_order_relaxed);
}

inline void count_pool_alloc() noexcept {
    g_pool_alloc.fetch_add(1, std::memory_order_relaxed);
}

inline void count_pool_reuse() noexcept {
    g_pool_reuse.fetch_add(1, std::memory_order_relaxed);
}

inline void reset_counters() noexcept {
    g_new_count.store(0, std::memory_order_relaxed);
    g_pool_alloc.store(0, std::memory_order_relaxed);
    g_pool_reuse.store(0, std::memory_order_relaxed);
}

}  // namespace arena::base

// 把全局 new 换成带计数的版本。
// 为什么写在头文件里而不是 .cpp：operator new 是弱符号链接，写在静态库里
// 有可能被连接器丢掉（尤其是 MSVC 的 /OPT:REF）；写在头文件、由每个 TU 包含，
// 就没有这个问题。代价是每个包含它的 TU 都会替换一次 —— 这是故意的。
// 注意：这个宏只该在压测程序 / 单测里的计数版本开启，业务库里别默认开。
#ifdef ARENA_COUNT_GLOBAL_NEW
#    include <new>

void* operator new(std::size_t size) {
    arena::base::count_new();
    if (size == 0) {
        size = 1;
    }
    void* p = std::malloc(size);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}

void operator delete(void* p) noexcept {
    std::free(p);
}

void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}
#endif
