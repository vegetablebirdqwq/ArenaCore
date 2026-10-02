// tools/ring_lab.cpp
//
// 一致性哈希实验台。两个实验：
//   A. 4 个分片扩到 5 个，取模分片 vs 一致性哈希，各有多少 key 要搬家。
//   B. 固定 10 个分片，虚拟节点数从 1 加到 2000，看数据倾斜怎么变。
//
// 预期结果（教程 §6.1）：
//   实验 A：取模迁移率 ≈80%，一致性哈希 ≈20%（新节点应承担的那一份）
//   实验 B：虚拟节点 1 → 最热节点 4.15x 均值；500 → 1.04x；2000 → 无收益

#include "base/consistent_hash.h"

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using arena::base::ConsistentHashRing;
using arena::base::hash64;

// 造一批"看起来随机但可复现"的 key。不用随机数是为了每次跑结果一样，
// 实验结论要能被别人重复出来。
std::vector<std::string> make_keys(std::size_t n) {
    std::vector<std::string> keys;
    keys.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned long long mixed = static_cast<unsigned long long>(i) * 2654435761ULL;
        keys.push_back("player:" + std::to_string(mixed % 1000000007ULL));
    }
    return keys;
}

// 最朴素的取模分片，用来做对照组。
int mod_shard(std::string_view key, int node_count) {
    return static_cast<int>(hash64(key) % static_cast<std::uint64_t>(node_count));
}

struct Skew {
    double max_over_avg = 0.0;
    double min_over_avg = 0.0;
    std::size_t max_load = 0;
    std::size_t min_load = 0;
};

// 把 key 全部分配一遍，统计各物理节点的负载。
Skew measure(const ConsistentHashRing& ring, const std::vector<std::string>& keys) {
    std::map<std::string, std::size_t> load;
    for (const std::string& k : keys) {
        ++load[ring.locate(k)];
    }
    std::size_t max_load = 0;
    std::size_t min_load = static_cast<std::size_t>(-1);
    std::size_t total = 0;
    for (const auto& entry : load) {
        max_load = entry.second > max_load ? entry.second : max_load;
        min_load = entry.second < min_load ? entry.second : min_load;
        total += entry.second;
    }
    const double avg = static_cast<double>(total) / static_cast<double>(load.empty() ? 1 : load.size());
    Skew s;
    s.max_over_avg = static_cast<double>(max_load) / avg;
    s.min_over_avg = static_cast<double>(min_load) / avg;
    s.max_load = max_load;
    s.min_load = min_load;
    return s;
}

}  // namespace

int main() {
    constexpr std::size_t kKeys = 200000;
    constexpr std::size_t kMigrateVnodes = 150;
    const std::vector<std::string> keys = make_keys(kKeys);

    // ---------------------------------------------------------------- 实验 A
    std::printf("=== 实验 A: 4 台分片扩到 5 台, %zu 个 key 里有多少要搬家 ===\n", kKeys);

    std::size_t moved_mod = 0;
    for (const std::string& k : keys) {
        if (mod_shard(k, 4) != mod_shard(k, 5)) {
            ++moved_mod;
        }
    }
    std::printf("[取模]     迁移率 = %6.2f%%    (理论值 80%%)\n",
                100.0 * static_cast<double>(moved_mod) / static_cast<double>(kKeys));

    ConsistentHashRing ring4;
    ConsistentHashRing ring5;
    for (int i = 0; i < 4; ++i) {
        ring4.add_node("logic-" + std::to_string(i), kMigrateVnodes);
    }
    for (int i = 0; i < 5; ++i) {
        ring5.add_node("logic-" + std::to_string(i), kMigrateVnodes);
    }

    std::size_t moved_ring = 0;
    for (const std::string& k : keys) {
        if (ring4.locate(k) != ring5.locate(k)) {
            ++moved_ring;
        }
    }
    std::printf("[一致性哈希] 迁移率 = %6.2f%%    (理论值 20%%)\n",
                100.0 * static_cast<double>(moved_ring) / static_cast<double>(kKeys));

    // 再看一眼：扩容之后，新节点 logic-4 到底从别人嘴里抢走了多少。
    std::size_t to_new_node = 0;
    for (const std::string& k : keys) {
        if (ring5.locate(k) == "logic-4") {
            ++to_new_node;
        }
    }
    std::printf("              环上点数: 4 台 = %zu, 5 台 = %zu\n",
                ring4.vnode_total(), ring5.vnode_total());
    std::printf("              新节点 logic-4 分到 %.2f%% 的 key (理想值 20%%)\n",
                100.0 * static_cast<double>(to_new_node) / static_cast<double>(kKeys));

    // ---------------------------------------------------------------- 实验 B
    std::printf("\n=== 实验 B: 10 台分片, 虚拟节点数 vs 数据倾斜 ===\n");
    std::printf("%-12s %-14s %-14s %-10s %-10s\n",
                "虚拟节点数", "最大负载/均值", "最小负载/均值", "最热节点", "最冷节点");
    const std::size_t vnode_candidates[] = {1, 5, 20, 50, 150, 500, 2000};
    for (const std::size_t vnodes : vnode_candidates) {
        ConsistentHashRing ring;
        for (int i = 0; i < 10; ++i) {
            ring.add_node("logic-" + std::to_string(i), vnodes);
        }
        const Skew s = measure(ring, keys);
        std::printf("%-12zu %-14.3f %-14.3f %-10zu %-10zu\n",
                    vnodes, s.max_over_avg, s.min_over_avg, s.max_load, s.min_load);
    }

    std::printf("\n注: 环上总点数 = 物理节点数 x 虚拟节点数, 查找是 O(log 环上点数)。\n");
    return 0;
}
