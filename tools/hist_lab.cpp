// tools/hist_lab.cpp
//
// 两个实验：
//   A. 单线程语义验证：造一批"绝大多数很快、极少数极慢"的样本，
//      对比平均值和 P50/P90/P99 —— 亲手看一次平均值是怎么骗人的。
//   B. 并发写入吞吐：16 个线程同时打同一个直方图，
//      对齐（每个桶独占 cache line）vs 不对齐，量出伪共享的代价。

#include "base/histogram.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <random>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

// 实验 A：桶宽 100us，1024 个桶 → 量程 0 ~ 102.3ms。
using FineHist = arena::base::LatencyHistogram<1024, 100>;

void print_hist(const char* tag, const FineHist& h) {
    std::printf("%-6s 样本数=%-8llu 平均=%8.1fus  P50=%8.1fus  P90=%8.1fus  P99=%9.1fus  越界占比=%.2f%%\n",
                tag,
                static_cast<unsigned long long>(h.count()),
                h.average(),
                h.percentile(50.0),
                h.percentile(90.0),
                h.percentile(99.0),
                h.overflow_ratio() * 100.0);
}

void experiment_a() {
    std::printf("=== 实验 A: 平均值 vs 分位（单位 us）===\n");
    FineHist h;
    // 989 个 1000us 的正常请求 + 11 个 100000us 的灾难请求，共 1000 个样本。
    for (int i = 0; i < 989; ++i) {
        h.record(1000);
    }
    for (int i = 0; i < 11; ++i) {
        h.record(100000);
    }
    print_hist("混合", h);

    FineHist only_fast;
    for (int i = 0; i < 989; ++i) {
        only_fast.record(1000);
    }
    print_hist("只算正常", only_fast);

    std::printf("-> 平均 2.09ms 看着还行，实际 1%% 的用户在等 100ms；\n");
    std::printf("-> P50/P90 报 1050us 而不是 1000us：那是桶宽 100us 带来的量化误差（±半个桶），\n");
    std::printf("   所以报延迟不要写小数位，写 'P99 ≈ 100ms' 就对了。\n\n");
}

// 实验 B 的第一部分：最纯粹的伪共享 —— 16 个计数器，线程 t 只写第 t 个。
// 注意 alignas 必须加在"元素类型"上：写成 `alignas(64) std::atomic<uint64_t> v[16]`
// 只保证数组首地址对齐，元素之间还是 8 字节紧挨着，等于没加。
struct alignas(64) AlignedCell {
    std::atomic<std::uint64_t> v{0};
};
struct PackedCell {
    std::atomic<std::uint64_t> v{0};
};
struct AlignedCounters {
    AlignedCell v[16];
};
struct PackedCounters {
    PackedCell v[16];
};

template <typename T>
double bench_counters(std::size_t threads, std::size_t per_thread) {
    T counters{};
    std::vector<std::thread> pool;
    pool.reserve(threads);
    const auto t0 = Clock::now();
    for (std::size_t t = 0; t < threads; ++t) {
        pool.emplace_back([&counters, t, per_thread]() {
            for (std::size_t i = 0; i < per_thread; ++i) {
                counters.v[t].v.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (std::thread& th : pool) {
        th.join();
    }
    const double sec = std::chrono::duration<double>(Clock::now() - t0).count();
    const std::size_t total = threads * per_thread;
    std::printf("    总样本=%-10zu 耗时=%6.3fs 吞吐=%6.1f M ops/s (单次 %.1f ns)\n",
                total, sec, static_cast<double>(total) / sec / 1e6,
                sec * 1e9 / static_cast<double>(total / threads));
    return static_cast<double>(total) / sec / 1e6;
}

// 实验 B 的第二部分：直接拿直方图类来压，看它快不快得起来。
template <bool kPad>
double bench_histogram(std::size_t threads, std::size_t per_thread) {
    using Hist = arena::base::LatencyHistogram<512, 1, kPad>;
    Hist hist;
    std::vector<std::thread> pool;
    pool.reserve(threads);
    const auto t0 = Clock::now();
    for (std::size_t t = 0; t < threads; ++t) {
        pool.emplace_back([&hist, t, per_thread]() {
            for (std::size_t i = 0; i < per_thread; ++i) {
                hist.record(t);
            }
        });
    }
    for (std::thread& th : pool) {
        th.join();
    }
    const double sec = std::chrono::duration<double>(Clock::now() - t0).count();
    const std::size_t total = threads * per_thread;
    std::printf("    sizeof(Bucket)=%-3zu 总样本=%-10zu 耗时=%6.3fs 吞吐=%6.1f M ops/s\n",
                kPad ? 64u : 8u, total, sec, static_cast<double>(total) / sec / 1e6);
    return static_cast<double>(total) / sec / 1e6;
}

// 正解：每个线程一个直方图，写完再合并。
double bench_per_thread_histogram(std::size_t threads, std::size_t per_thread) {
    using Hist = arena::base::LatencyHistogram<512, 1, true>;
    // LatencyHistogram 里有 std::atomic，不可拷贝不可移动，用 unique_ptr 管起来。
    std::vector<std::unique_ptr<Hist>> hists;
    hists.reserve(threads);
    for (std::size_t t = 0; t < threads; ++t) {
        hists.push_back(std::make_unique<Hist>());
    }

    std::vector<std::thread> pool;
    pool.reserve(threads);
    const auto t0 = Clock::now();
    for (std::size_t t = 0; t < threads; ++t) {
        pool.emplace_back([&hists, t, per_thread]() {
            Hist& mine = *hists[t];
            for (std::size_t i = 0; i < per_thread; ++i) {
                mine.record(t);
            }
        });
    }
    for (std::thread& th : pool) {
        th.join();
    }
    const double write_sec = std::chrono::duration<double>(Clock::now() - t0).count();

    const auto t1 = Clock::now();
    Hist total;
    for (const auto& h : hists) {
        total.merge_from(*h);
    }
    const double merge_sec = std::chrono::duration<double>(Clock::now() - t1).count();

    const std::size_t n = threads * per_thread;
    std::printf("    写入 %6.3fs (%.1f M ops/s)，合并 %zu 个直方图 %.3fs，校验 count=%llu\n",
                write_sec, static_cast<double>(n) / write_sec / 1e6, threads, merge_sec,
                static_cast<unsigned long long>(total.count()));
    return static_cast<double>(n) / (write_sec + merge_sec) / 1e6;
}

void experiment_b() {
    constexpr std::size_t kThreads = 16;
    constexpr std::size_t kPerThread = 10000000;
    std::printf("=== 实验 B: %zu 线程并发写（每线程 1000 万次，-O2）===\n", kThreads);

    std::printf("[B1] 纯计数器，看伪共享本身有多少代价\n");
    std::printf("  对齐(64B/个): ");
    const double c_aligned = bench_counters<AlignedCounters>(kThreads, kPerThread);
    std::printf("  不对齐(8B/个): ");
    const double c_packed = bench_counters<PackedCounters>(kThreads, kPerThread);
    std::printf("  -> 伪共享代价 = %.2fx\n\n", c_aligned / c_packed);

    std::printf("[B2] 换成我写的 LatencyHistogram 呢？\n");
    std::printf("  对齐(64B/桶): ");
    const double h_aligned = bench_histogram<true>(kThreads, kPerThread);
    std::printf("  不对齐(8B/桶): ");
    const double h_packed = bench_histogram<false>(kThreads, kPerThread);
    std::printf("  -> 对齐/不对齐 = %.2fx\n\n", h_aligned / h_packed);

    std::printf("[B3] 换成正解：每线程一个直方图，最后 merge_from\n");
    const double per_thread = bench_per_thread_histogram(kThreads, kPerThread);
    std::printf("  -> 相比 B2，吞吐提升 %.2fx。完整统计（含合并）还是比「所有人抢一个」快。\n",
                per_thread / h_aligned);

    std::printf("\n-> 这就是「先测量再改」的价值：B1 里对齐是 %.2fx 的差距，\n",
                c_aligned / c_packed);
    std::printf("   但 B2 真拿 record() 来压，对齐几乎没用（%.2fx）—— 因为 record() 里除了桶，\n",
                h_aligned / h_packed);
    std::printf("   还有一个所有线程共享的 sum_us_（算平均值用），那才是真正的瓶颈。\n");
    std::printf("-> 而 B3 说明：正确的改法不是继续抠对齐，而是换结构。\n");
    std::printf("-> 数字会随机器负载浮动，跑三次取中位数再看结论。\n");
}

}  // namespace

int main() {
    experiment_a();
    experiment_b();
    return 0;
}
