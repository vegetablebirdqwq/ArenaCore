#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace arena::base {

/// murmur3 的收尾混合（finalizer）。**这一步不能省**。
///
/// FNV-1a 的最后一步是"乘一个常数"，它对**输入末尾字节**的扩散极差：
/// 两个只差最后一个字符的字符串（比如 "logic-3#0" 和 "logic-3#1"），
/// 哈希结果只差约 1.1e12，而整个 64 位空间是 1.8e19 —— 相对差 6e-8。
/// 后果是：同一个物理节点的 150 个虚拟节点全挤在一个针尖大的区间里，
/// 虚拟节点等于白加，数据倾斜一点没改善。
inline std::uint64_t mix64(std::uint64_t x) noexcept {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

/// FNV-1a 64 位哈希 + finalizer。跨平台、跨编译器结果完全一致。
///
/// 为什么不用 std::hash<std::string>：标准没规定它怎么算，
/// 换一个 STL 版本分布就可能变。分片这件事最怕的就是
/// "同一批 key 昨天落在 logic-2、今天落在 logic-3"，
/// 那等于每次升级都做一次全量迁移。所以自己写，可控。
inline std::uint64_t hash64(std::string_view s) noexcept {
    std::uint64_t h = 14695981039346656037ULL;  // FNV offset basis
    for (const char ch : s) {
        h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(ch));
        h *= 1099511628211ULL;  // FNV prime
    }
    return mix64(h);
}

/// 一致性哈希环。
///
/// 它解决什么问题：分片节点数变了以后，尽量少搬数据。
/// 取模分片在 4 → 5 台时会有约 80% 的 key 换家（第 1 节用实验证明），
/// 一致性哈希把"节点数"从哈希函数里拿掉，只让 key 和节点各自落到同一个环上，
/// 增减节点只影响环上相邻的那一段。
///
/// 实现选取：std::map 有序 + lower_bound。
/// 分片节点就几十个（几十物理节点 × 几百虚拟节点 = 上万个环上点），
/// 一次查找 14 次比较，比"排好序的 vector + 二分"差的是缓存命中率而不是复杂度。
/// 只有在"每次玩家移动都要查一次分片"的极端场景才值得换成 vector 版本。
class ConsistentHashRing {
public:
    /// 插入一个物理节点，同时帮它生成 vnodes 个虚拟节点。
    /// 幂等：同一个节点重复插入不会重复建环。
    void add_node(const std::string& node, std::size_t vnodes = 150) {
        if (vnodes == 0) {
            vnodes = 1;
        }
        if (vnode_count_.find(node) != vnode_count_.end()) {
            return;
        }
        vnode_count_[node] = vnodes;
        for (std::size_t i = 0; i < vnodes; ++i) {
            // 虚拟节点的 key 必须是"节点名 + 序号"，不能用随机数：
            // 否则同一个节点两次 add_node 得到的环不一样，删不掉也复现不了。
            ring_.emplace(hash64(vnode_key(node, i)), node);
        }
    }

    /// 摘掉一个物理节点。哈希是确定的，所以能按同样的算法把它的虚拟节点全找回来。
    void remove_node(const std::string& node) {
        const auto it = vnode_count_.find(node);
        if (it == vnode_count_.end()) {
            return;
        }
        for (std::size_t i = 0; i < it->second; ++i) {
            ring_.erase(hash64(vnode_key(node, i)));
        }
        vnode_count_.erase(it);
    }

    /// key 归哪个物理节点管。环空时返回空串，调用方必须处理。
    std::string locate(std::string_view key) const {
        if (ring_.empty()) {
            return {};
        }
        const std::uint64_t h = hash64(key);
        auto it = ring_.lower_bound(h);  // 顺时针找到第一个虚拟节点
        if (it == ring_.end()) {
            it = ring_.begin();          // 落到最后一个虚拟节点后面，回绕到环首
        }
        return it->second;
    }

    std::size_t node_count() const noexcept { return vnode_count_.size(); }
    std::size_t vnode_total() const noexcept { return ring_.size(); }
    bool empty() const noexcept { return ring_.empty(); }
    std::size_t vnodes_of(const std::string& node) const {
        const auto it = vnode_count_.find(node);
        return it == vnode_count_.end() ? 0 : it->second;
    }

private:
    static std::string vnode_key(const std::string& node, std::size_t i) {
        return node + "#" + std::to_string(i);
    }

    std::map<std::uint64_t, std::string> ring_;       // 环：hash → 物理节点
    std::map<std::string, std::size_t> vnode_count_;  // 物理节点 → 虚拟节点数
};

}  // namespace arena::base
