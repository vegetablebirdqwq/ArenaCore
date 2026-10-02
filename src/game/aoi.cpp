#include "game/aoi.h"

#include <algorithm>

namespace arena::game {

void AoiGrid::erase(std::uint32_t id) {
    const auto it = where_.find(id);
    if (it == where_.end()) {
        return;
    }
    const auto cell_it = cells_.find(it->second);
    if (cell_it != cells_.end()) {
        auto& ids = cell_it->second.ids;
        ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
        if (ids.empty()) {
            cells_.erase(cell_it);   // 空格子及时清掉，否则地图走一圈会把抖动的格子都留下
        }
    }
    where_.erase(it);
}

void AoiGrid::insert(std::uint32_t id, std::int32_t x_fp, std::int32_t y_fp) {
    const std::int64_t key = cell_key(to_cell(x_fp), to_cell(y_fp));

    const auto it = where_.find(id);
    if (it != where_.end()) {
        if (it->second == key) {
            return;              // 没换格，什么都不用做 —— 这就是快路径
        }
        // 换格了：从旧格子里摘掉自己，再挂到新格子。
        // 这里不能调 erase(id)，因为那会把 where_ 里的记录也删掉，
        // 而我们要保留，避免多一次哈希查找。
        const auto old_cell = cells_.find(it->second);
        if (old_cell != cells_.end()) {
            auto& ids = old_cell->second.ids;
            ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
            if (ids.empty()) {
                cells_.erase(old_cell);
            }
        }
        it->second = key;
    } else {
        where_.emplace(id, key);
    }

    cells_[key].ids.push_back(id);
}

void AoiGrid::query_neighbors(std::int32_t x_fp,
                              std::int32_t y_fp,
                              std::uint32_t exclude_id,
                              std::vector<std::uint32_t>& out) const {
    out.clear();
    const std::int32_t cx = to_cell(x_fp);
    const std::int32_t cy = to_cell(y_fp);

    for (std::int32_t dy = -1; dy <= 1; ++dy) {
        for (std::int32_t dx = -1; dx <= 1; ++dx) {
            const auto it = cells_.find(cell_key(cx + dx, cy + dy));
            if (it == cells_.end()) {
                continue;      // 空格子直接跳过，省 8 次遍历
            }
            for (const std::uint32_t id : it->second.ids) {
                if (id != exclude_id) {
                    out.push_back(id);
                }
            }
        }
    }
}

}  // namespace arena::game
