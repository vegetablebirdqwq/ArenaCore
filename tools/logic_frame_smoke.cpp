// tools/logic_frame_smoke.cpp
// 固定帧验证：跑 1 秒逻辑，统计实际推进的帧数，应接近 30 帧/秒。
// 同时验证死亡螺旋防护：手动注入一次 2 秒的"假卡顿"，确认只补 5 帧而不是 60 帧。
#include "game/room.h"

#include <chrono>
#include <cstdio>
#include <thread>

int main() {
    using namespace std::chrono;

    arena::game::Room room;

    // 阶段 1：正常跑 1 秒，看帧率
    const auto t0 = steady_clock::now();
    while (steady_clock::now() - t0 < seconds(1)) {
        room.tick(steady_clock::now());
        std::this_thread::sleep_for(milliseconds(1));
    }
    std::printf("1 秒内推进 %llu 帧（预期接近 30）\n",
                static_cast<unsigned long long>(room.frame()));

    // 阶段 2：模拟一次 2 秒的调度抖动（卡顿）
    std::this_thread::sleep_for(seconds(2));
    const auto before = room.frame();
    room.tick(steady_clock::now());   // 这次 tick 的 elapsed 约 2 秒
    const auto after = room.frame();
    std::printf("注入 2 秒卡顿后：只补了 %llu 帧（夹紧上限=5，不是 60）\n",
                static_cast<unsigned long long>(after - before));

    // 阶段 3：再正常跑 1 秒，确认节奏恢复
    const auto before2 = room.frame();
    const auto t1 = steady_clock::now();
    while (steady_clock::now() - t1 < seconds(1)) {
        room.tick(steady_clock::now());
        std::this_thread::sleep_for(milliseconds(1));
    }
    std::printf("恢复后 1 秒推进 %llu 帧\n",
                static_cast<unsigned long long>(room.frame() - before2));
    return 0;
}
