#include "game/room.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace arena::game {

void Room::tick(Clock::time_point now) {
    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(now - last_).count();
    last_ = now;

    // ① 夹紧：单次补帧最多追 5 帧（166ms）的欠账。
    //    如果不夹，一次 GC/换页/调度抖动让 elapsed 变成 2 秒，
    //    while 循环要连补 60 帧，而这 60 帧又要花掉 >2 秒 ——
    //    欠账越补越多，这就是"死亡螺旋"(spiral of death)。
    //    宁可把时间丢掉（客户端看起来是一次卡顿），也不能把服务器拖死。
    if (elapsed_ms > 5.0 * kFrameMs) {
        acc_ms_ = 0.0;                     // 丢掉这段异常时间
        last_ = now;
        do_frame();                        // 至少推进一帧，保持心跳
        return;
    }

    acc_ms_ += elapsed_ms;

    // ② 补帧：欠够一帧就补一帧，可能补多帧。
    //    注意这里的比较是 double，33.333 这种数不能用 == 判等。
    constexpr double kEps = 1e-9;
    while (acc_ms_ + kEps >= kFrameMs) {
        acc_ms_ -= kFrameMs;
        do_frame();
    }
}

void Room::do_frame() {
    ++frame_;
    // 逻辑帧的具体内容（收包→移动→技能→AOI→快照→发送）在后续步骤接入。
    // 当前阶段只验证固定帧节奏本身。
    if ((frame_ % 300) == 0) {
        std::printf("[room] frame %llu\n", static_cast<unsigned long long>(frame_));
    }
}

}  // namespace arena::game
