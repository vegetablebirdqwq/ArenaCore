#include "net/session.h"

#include <cstdio>

namespace arena::net {

Session::Session(IocpService& io, SOCKET sock, std::uint64_t id, std::string peer_text)
    : io_(io), sock_(sock), id_(id), peer_text_(std::move(peer_text)) {
    // 关键：把新 socket 绑到完成端口。完成端口关联是每个句柄一份的，
    // 不通过父子关系继承 —— 所以每个 Session 都要显式绑一次。
    if (!io_.attach(sock_, reinterpret_cast<ULONG_PTR>(this))) {
        std::printf("[session %llu] 绑定完成端口失败, err=%d\n",
                    static_cast<unsigned long long>(id_), ::GetLastError());
        // 绑不上就当这个连接不存在：关掉它
        ::closesocket(sock_);
        sock_ = INVALID_SOCKET;
        return;
    }
}

Session::~Session() {
    // 析构时确保 socket 关闭、self_keepalive_ 已释放（若还没关过）
    shutdown_socket();
}

void Session::start() {
    // 登记自引用：让对象在自己还有 IO 在飞的时候活着。
    // 注意：必须在 shared_ptr 建立之后调（shared_from_this 依赖它）。
    std::lock_guard<std::mutex> lk(life_mutex_);
    if (!self_keepalive_) {
        self_keepalive_ = shared_from_this();
    }
    // 收数据的完整链路（do_recv / WSARecv）在下一步接入，
    // 当前阶段 start() 只负责让连接进入"活跃且被持有"状态。
}

void Session::send(std::vector<std::uint8_t> /*packet*/) {
    // 发送链路（写队列 + WSASend）在下一步接入。
}

void Session::close() {
    if (closed_.exchange(true)) {
        return;
    }
    // 通知业务层：连接关了
    if (on_close_) {
        on_close_(shared_from_this());
    }
    shutdown_socket();
    // 释放自引用，允许对象析构（如果有其他持有着，他们继续持有）
    std::lock_guard<std::mutex> lk(life_mutex_);
    self_keepalive_.reset();
}

void Session::close_when_drained() {
    // "发完再关"的完整逻辑在发送链路接入后实现；当前阶段直接 close。
    close();
}

void Session::on_io_completed(OVERLAPPED* /*ov*/, std::uint32_t /*bytes*/,
                              bool success, int error) {
    // 完整收发处理（handle_recv / handle_send）在下一步接入。
    // 当前阶段：只要发现 IO 出错就关连接，保证没有泄漏的完成包悬着。
    if (!success) {
        handle_io_error("io 完成但失败", error);
    }
}

void Session::touch() noexcept {
    last_active_us_.store(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count(),
        std::memory_order_relaxed);
}

std::chrono::steady_clock::time_point Session::last_active() const noexcept {
    const auto us = last_active_us_.load(std::memory_order_relaxed);
    return std::chrono::steady_clock::time_point(std::chrono::microseconds(us));
}

bool Session::add_io_ref() {
    std::lock_guard<std::mutex> lk(life_mutex_);
    if (closed_.load(std::memory_order_acquire)) {
        return false;
    }
    ++pending_io_;
    return true;
}

void Session::drop_io_ref() {
    bool last = false;
    {
        std::lock_guard<std::mutex> lk(life_mutex_);
        --pending_io_;
        last = (pending_io_ == 0);
    }
    if (last) {
        self_keepalive_.reset();   // 没有 IO 在飞了，允许析构
    }
}

void Session::handle_io_error(const char* what, int error) {
    if (error != ERROR_OPERATION_ABORTED && !closed_.load(std::memory_order_acquire)) {
        std::printf("[session %llu] %s, err=%d (%s)\n",
                    static_cast<unsigned long long>(id_), what, error, peer_text_.c_str());
    }
    close();
}

void Session::shutdown_socket() {
    if (sock_ != INVALID_SOCKET) {
        // 取消在途 IO + 关 socket。重复调用是幂等的。
        ::CancelIoEx(reinterpret_cast<HANDLE>(sock_), nullptr);
        ::closesocket(sock_);
        sock_ = INVALID_SOCKET;
    }
}

}  // namespace arena::net
