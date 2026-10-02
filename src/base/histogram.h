#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace arena::base {

/// 定宽延迟直方图（latency histogram）。
///
/// 它解决什么问题：平均值会把长尾吃掉。
/// 989 个请求 1ms、11 个请求 100ms，平均只有 2.09ms，看着一切正常；
/// 但 P99 是 100ms —— 那 1% 的玩家在骂人。平均值骗人的地方就在这里：
/// 它把"少数人的灾难"和"多数人的正常"搅成一锅，抹掉了分布形状。
/// 所以延迟指标只认 P50 / P95 / P99，平均值只用来做粗筛。
///
/// 三个设计取舍，面试都会被问：
///  1. 为什么定宽桶而不是精确记录每个样本：精确记录要存全部样本，
///     1 万 QPS 跑 10 分钟就是 600 万个数字，还要排序才能算分位 ——
///     监控数据不需要那么精确，直方图内存固定、O(桶数) 出分位。
///  2. 为什么每个桶 alignas(64)：伪共享（见 hist_lab 实验）。
///  3. 为什么用 memory_order_relaxed：计数只需要"加一"这个动作原子，
///     不需要它给别的内存当同步点。
template <std::size_t kBuckets = 512, std::uint64_t kWidth = 500, bool kPadBuckets = true>
class LatencyHistogram {
    static_assert(kBuckets >= 2, "至少要有 1 个正常桶 + 1 个溢出桶");

public:
    static constexpr std::uint64_t kBucketWidth = kWidth;
    /// 大于等于这个值的样本全部进溢出桶（最后一个桶）。
    static constexpr std::uint64_t kOverflowFrom = (kBuckets - 1) * kWidth;

    /// 记一个样本。单位必须和 kWidth 一致（本文件统一用微秒）。
    void record(std::uint64_t value_us) noexcept {
        std::size_t idx = static_cast<std::size_t>(value_us / kWidth);
        if (idx >= kBuckets - 1) {
            idx = kBuckets - 1;
        }
        buckets_[idx].value.fetch_add(1, std::memory_order_relaxed);
        sum_us_.fetch_add(value_us, std::memory_order_relaxed);
    }

    std::uint64_t count() const noexcept {
        std::uint64_t total = 0;
        for (const Bucket& b : buckets_) {
            total += b.value.load(std::memory_order_relaxed);
        }
        return total;
    }

    double average() const noexcept {
        const std::uint64_t n = count();
        if (n == 0) {
            return 0.0;
        }
        return static_cast<double>(sum_us_.load(std::memory_order_relaxed)) / static_cast<double>(n);
    }

    /// 分位点，p ∈ (0, 100]。桶内按均匀分布线性插值，
    /// 所以结果比"直接返回桶下界"准，误差不超过一个桶宽。
    double percentile(double p) const {
        const std::uint64_t total = count();
        if (total == 0) {
            return 0.0;
        }
        const double target = p / 100.0 * static_cast<double>(total);
        std::uint64_t cum = 0;
        for (std::size_t i = 0; i < kBuckets; ++i) {
            const std::uint64_t c = buckets_[i].value.load(std::memory_order_relaxed);
            if (c == 0) {
                continue;
            }
            const std::uint64_t prev = cum;
            cum += c;
            if (static_cast<double>(cum) >= target) {
                if (i == kBuckets - 1) {
                    return static_cast<double>(kOverflowFrom);  // 溢出桶只能给下界，别当天花板用
                }
                const double frac =
                    (target - static_cast<double>(prev)) / static_cast<double>(c);
                return (static_cast<double>(i) + frac) * static_cast<double>(kWidth);
            }
        }
        return static_cast<double>(kOverflowFrom);
    }

    /// 溢出桶占比。这个数字本身就该报警：它大于 1% 说明
    /// "我的直方图已经量不到我系统的尾巴了"，该换更大的量程。
    double overflow_ratio() const {
        const std::uint64_t total = count();
        if (total == 0) {
            return 0.0;
        }
        return static_cast<double>(buckets_[kBuckets - 1].value.load(std::memory_order_relaxed)) /
               static_cast<double>(total);
    }

    /// 把另一个（通常是某个线程私有的）直方图并进来。
    /// 用途：每线程各记各的（无锁无竞争），最后汇总出全局分位。
    void merge_from(const LatencyHistogram& other) noexcept {
        for (std::size_t i = 0; i < kBuckets; ++i) {
            const std::uint64_t v = other.buckets_[i].value.load(std::memory_order_relaxed);
            if (v != 0) {
                buckets_[i].value.fetch_add(v, std::memory_order_relaxed);
            }
        }
        sum_us_.fetch_add(other.sum_us_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }

    void reset() noexcept {
        for (Bucket& b : buckets_) {
            b.value.store(0, std::memory_order_relaxed);
        }
        sum_us_.store(0, std::memory_order_relaxed);
    }

    /// 打印 percentile(p) == 0 的桶索引，调试用：确认自己的量程选对了。
    std::size_t used_buckets() const noexcept {
        std::size_t n = 0;
        for (const Bucket& b : buckets_) {
            if (b.value.load(std::memory_order_relaxed) != 0) {
                ++n;
            }
        }
        return n;
    }

private:
    // alignas(kPadBuckets ? 64 : 8) —— 这个开关就是为了让你能亲手量出伪共享的代价，
    // 把 kPadBuckets 设成 false 再跑一次 hist_lab，性能对比就在眼前。
    struct alignas(kPadBuckets ? 64 : 8) Bucket {
        std::atomic<std::uint64_t> value{0};
    };

    Bucket buckets_[kBuckets]{};
    std::atomic<std::uint64_t> sum_us_{0};
};

/// 常用实例：逻辑帧 tick 耗时（100us 一档，量程 0 ~ 25.5ms，30Hz 的预算是 33.3ms）。
using TickHistogram = LatencyHistogram<256, 100>;

/// 下行消息 RTT（500us 一档，量程 0 ~ 255.5ms）。
using RttHistogram = LatencyHistogram<512, 500>;

}  // namespace arena::base
