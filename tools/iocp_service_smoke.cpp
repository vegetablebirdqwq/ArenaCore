// tools/iocp_service_smoke.cpp
// IocpService 冒烟测试：起 worker → post 10000 个任务 → 等全部跑完 → 停止。
// 预期输出：10000 个任务跑完了，worker 数 = N（N = 逻辑核数）
#include "net/iocp_service.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

int main() {
    arena::net::IocpService io(0);   // 0 = 核数
    io.start();

    std::atomic<int> counter{0};
    for (int i = 0; i < 10000; ++i) {
        io.post([&counter, i] {
            counter.fetch_add(1, std::memory_order_relaxed);
            if (i == 9999) {
                std::printf("最后一个任务在 线程 %lu 上跑\n", ::GetCurrentThreadId());
            }
        });
    }

    // 等任务跑完
    while (counter.load() < 10000) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::printf("10000 个任务跑完了，worker 数 = %u\n", io.worker_count());
    io.stop();
    std::printf("主线程: %lu\n", ::GetCurrentThreadId());
    return 0;
}
