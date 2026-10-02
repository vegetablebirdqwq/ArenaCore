#include "net/iocp_service.h"

#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>

namespace arena::net {

IocpService::IocpService(std::uint32_t worker_count) : worker_count_(worker_count) {
    if (worker_count_ == 0) {
        worker_count_ = std::thread::hardware_concurrency();
        if (worker_count_ == 0) {
            worker_count_ = 4;   // 极少数环境下 hardware_concurrency 会返回 0，兜个底
        }
    }

    // 用法 A：新建端口。第 4 个参数就是并发值。
    iocp_ = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, worker_count_);
    if (iocp_ == nullptr) {
        throw std::runtime_error("CreateIoCompletionPort 失败, err=" +
                                 std::to_string(::GetLastError()));
    }
    std::printf("[iocp] 端口建好了，worker 数 = %u\n", worker_count_);
}

IocpService::~IocpService() {
    stop();
}

void IocpService::start() {
    if (running_.exchange(true)) {
        return;   // 已经起过了
    }
    workers_.reserve(worker_count_);
    for (std::uint32_t i = 0; i < worker_count_; ++i) {
        workers_.emplace_back([this] { worker_loop(); });
    }
}

bool IocpService::attach(SOCKET sock, ULONG_PTR key) {
    // 用法 B：把句柄绑到已有端口。成功时返回的就是 iocp_ 本身。
    const HANDLE ret = ::CreateIoCompletionPort(reinterpret_cast<HANDLE>(sock), iocp_, key, 0);
    return ret != nullptr;
}

bool IocpService::post(Task task) {
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }
    // 把闭包放到堆上，用 OVERLAPPED* 这个字段当"任意指针"捎过去。
    // 注意：这里绝对不能传 nullptr —— worker 会把它当成 stop 信号。
    auto* heap_task = new Task(std::move(task));
    if (::PostQueuedCompletionStatus(iocp_, 0, kKeyTask,
                                     reinterpret_cast<LPOVERLAPPED>(heap_task)) == FALSE) {
        delete heap_task;
        return false;
    }
    return true;
}

void IocpService::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    // 给每条 worker 塞一个"下班"包。别直接 CloseHandle —— 那样在跑的 worker 会崩。
    for (std::size_t i = 0; i < workers_.size(); ++i) {
        ::PostQueuedCompletionStatus(iocp_, 0, kKeyStop, nullptr);
    }
    for (auto& t : workers_) {
        if (t.joinable()) {
            t.join();
        }
    }
    workers_.clear();

    if (iocp_ != nullptr) {
        ::CloseHandle(iocp_);
        iocp_ = nullptr;
    }
    std::printf("[iocp] 已停止\n");
}

void IocpService::worker_loop() {
    while (running_.load(std::memory_order_acquire)) {
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        OVERLAPPED* ov = nullptr;

        // 用 500ms 超时版本，而不是 INFINITE：
        // 这样即使某个 stop 包丢了，worker 也能在一秒内自己发现 running_ 变成 false 并退出。
        const BOOL ok = ::GetQueuedCompletionStatus(iocp_, &bytes, &key, &ov, 500);

        if (ov == nullptr) {
            if (key == kKeyStop) {
                break;   // stop() 叫我们下班
            }
            if (!ok && ::GetLastError() == WAIT_TIMEOUT) {
                continue;   // 啥也没有，回去看一眼 running_ 再决定要不要继续
            }
            break;   // 端口被关了（ERROR_ABANDONED_WAIT_0），只能退
        }

        if (key == kKeyTask) {
            // post() 塞进来的闭包，用 unique_ptr 保证异常安全
            std::unique_ptr<Task> task(reinterpret_cast<Task*>(ov));
            (*task)();
            continue;
        }

        completion_count_.fetch_add(1, std::memory_order_relaxed);

        // 剩下的都是业务对象。调用它的虚函数。
        auto* handler = reinterpret_cast<CompletionHandler*>(key);
        handler->on_io_completed(ov, bytes, ok != FALSE,
                                 ok != FALSE ? 0 : static_cast<int>(::GetLastError()));
    }
}

}  // namespace arena::net
