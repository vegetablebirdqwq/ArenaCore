#pragma once

#include <chrono>
#include <cstdint>

namespace arena::game {

using Clock = std::chrono::steady_clock;

// 逻辑帧间隔，单位毫秒。30Hz => 1000/30 = 33.3333...
inline constexpr double kFrameMs = 1000.0 / 30.0;

// 单帧 tick 的绝对上限。不是"目标"，是"熔断阈值"。
inline constexpr double kFrameBudgetMs = 12.0;

class Room {
public:
    void tick(Clock::time_point now);

    std::uint64_t frame() const noexcept { return frame_; }

private:
    void do_frame();

    Clock::time_point last_ = Clock::now();
    double acc_ms_ = 0.0;          // 累加器：欠了多少毫秒的逻辑时间
    std::uint64_t frame_ = 0;
};

}  // namespace arena::game
