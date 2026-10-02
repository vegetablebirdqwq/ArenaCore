// tools/aoi_smoke.cpp
// AOI 九宫格验证：
//   1. to_cell 负数边界（教程 §4.3 的 bug 抓取测试）
//   2. 九宫格邻居查询（含排除自己）
//   3. 换格快路径（insert 同一格不重复）
#include "game/aoi.h"

#include <cstdio>
#include <vector>

static int failures = 0;
static void expect(const char* name, bool cond) {
    std::printf("%s: %s\n", cond ? "PASS" : "FAIL", name);
    if (!cond) ++failures;
}

int main() {
    using namespace arena::game;

    // ---- 1. to_cell 负数边界（教程 §4.3 的 bug 抓取测试）----
    expect("to_cell(0) == 0", to_cell(0) == 0);
    expect("to_cell(127*kFpOne) == 0", to_cell(127 * kFpOne) == 0);
    expect("to_cell(128*kFpOne) == 1", to_cell(128 * kFpOne) == 1);
    expect("to_cell(-1) == -1", to_cell(-1) == -1);
    expect("to_cell(-128*kFpOne) == -1", to_cell(-128 * kFpOne) == -1);
    expect("to_cell(-129*kFpOne) == -2", to_cell(-129 * kFpOne) == -2);

    // ---- 2. 九宫格邻居查询 ----
    AoiGrid grid;
    // 玩家 1 在原点；2 在 130 单位右（跨到格 1，九宫格内）；3 在 130 单位上（跨格）；4 在 400 单位远（格 3，九宫格外）
    grid.insert(1, 0, 0);
    grid.insert(2, 130 * kFpOne, 0);
    grid.insert(3, 0, 130 * kFpOne);
    grid.insert(4, 400 * kFpOne, 0);

    std::vector<std::uint32_t> out;
    grid.query_neighbors(0, 0, 1, out);
    std::printf("玩家 1 的邻居: ");
    for (auto id : out) std::printf("%u ", id);
    std::printf("\n");
    // 2（130 跨格到格 1，同九宫格）和 3（130 跨格到格 1，同九宫格）应在；4（400 → 格 3，不在九宫格）不应在
    expect("邻居含 2", std::find(out.begin(), out.end(), 2) != out.end());
    expect("邻居含 3", std::find(out.begin(), out.end(), 3) != out.end());
    expect("邻居不含 4", std::find(out.begin(), out.end(), 4) == out.end());
    expect("邻居不含自己 1", std::find(out.begin(), out.end(), 1) == out.end());

    // ---- 3. 换格快路径 ----
    grid.insert(1, 0, 0);                 // 同一格，应走快路径不重复
    grid.query_neighbors(0, 0, 1, out);   // 邻居数量应仍为 2（不含重复的 1）
    std::printf("重新 insert 后邻居数: %zu（应为 2）\n", out.size());
    expect("同一格重复 insert 不重复", out.size() == 2);

    // ---- 4. 换格 ----
    grid.insert(1, 500 * kFpOne, 0);      // 玩家 1 换到格 3
    grid.query_neighbors(0, 0, 2, out);   // 从原点看（排除 2）
    // 原点九宫格内现在只有 3；1 已离开
    expect("1 换格后原点九宫格无 1", std::find(out.begin(), out.end(), 1) == out.end());
    expect("原点九宫格仍有 3", std::find(out.begin(), out.end(), 3) != out.end());

    std::printf("\n%s（%d 个失败）\n", failures == 0 ? "全部通过" : "有失败", failures);
    return failures == 0 ? 0 : 1;
}
