#pragma once

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

#include "net/io_service.h"

namespace arena::net {

/// 完成键的保留值。大于 kKeyTagMax 的完成键，我们约定它是一个 CompletionHandler* 的地址。
inline constexpr ULONG_PTR kKeyStop = 1;
inline constexpr ULONG_PTR kKeyTask = 2;
inline constexpr ULONG_PTR kKeyAccept = 3;
inline constexpr ULONG_PTR kKeyTagMax = 0xFFFF;

/// 能被完成端口回调的对象。Session 继承它。
class CompletionHandler {
public:
    virtual ~CompletionHandler() = default;
    virtual void on_io_completed(OVERLAPPED* ov, std::uint32_t bytes, bool success, int error) = 0;
};

class IocpService final : public IoService {
public:
    /// worker_count 传 0 = 用 CPU 逻辑核数。
    explicit IocpService(std::uint32_t worker_count = 0);
    ~IocpService() override;

    IocpService(const IocpService&) = delete;
    IocpService& operator=(const IocpService&) = delete;

    void start() override;
    void stop() override;
    bool post(Task task) override;

    /// 把一个套接字绑到完成端口上。key 就是它以后每一个完成包的完成键。
    bool attach(SOCKET sock, ULONG_PTR key);

    HANDLE native_handle() const noexcept { return iocp_; }
    std::uint32_t worker_count() const noexcept { return worker_count_; }
    std::size_t outstanding_io_hint() const noexcept { return completion_count_.load(std::memory_order_relaxed); }

private:
    void worker_loop();

    HANDLE iocp_ = nullptr;
    std::vector<std::thread> workers_;
    std::atomic<bool> running_{false};
    std::uint32_t worker_count_ = 0;
    std::atomic<std::uint64_t> completion_count_{0};
};

}  // namespace arena::net
