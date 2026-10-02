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
        ::closesocket(sock_);
        sock_ = INVALID_SOCKET;
    }
}

Session::~Session() {
    shutdown_socket();
}

void Session::start() {
    // 开始收数据。必须在设置好回调之后调。
    // 这里不需要自己 add_io_ref() —— do_recv() 内部会加。
    do_recv();
}

void Session::send(std::vector<std::uint8_t> packet) {
    if (closed_.load(std::memory_order_acquire) || packet.empty()) {
        return;
    }

    bool kick = false;
    {
        std::lock_guard<std::mutex> lk(send_mutex_);
        send_queue_.push_back(SendChunk{std::move(packet), 0});
        if (!send_posted_ && !send_kicked_) {
            send_kicked_ = true;      // 认领"去发"这个任务
            kick = true;
        }
    }
    if (kick) {
        kick_send();                  // 在锁外 post，锁要短
    }
}

void Session::close() {
    {
        // 关闸。之后 add_io_ref() 一律返回 false，不会再投出新的 IO。
        std::lock_guard<std::mutex> lk(life_mutex_);
        if (closed_.exchange(true, std::memory_order_acq_rel)) {
            return;   // 已经关过了
        }
    }

    if (sock_ != INVALID_SOCKET) {
        // 取消所有未完成的 IO。注意用 CancelIoEx 不是 CancelIo：
        // CancelIo 只能取消【调用线程】发出的 IO，我们的 recv 和 send 还可能是不同线程发的。
        ::CancelIoEx(reinterpret_cast<HANDLE>(sock_), nullptr);
    }

    bool immediate = false;
    {
        std::lock_guard<std::mutex> lk(life_mutex_);
        immediate = (pending_io_ == 0);
    }
    if (immediate) {
        std::shared_ptr<Session> guard = shared_from_this();
        shutdown_socket();
    }
}

void Session::close_when_drained() {
    bool empty = false;
    {
        std::lock_guard<std::mutex> lk(send_mutex_);
        drain_then_close_ = true;
        empty = send_queue_.empty() && !send_posted_;
    }
    if (empty) {
        close();
    }
}

void Session::on_io_completed(OVERLAPPED* ov, std::uint32_t bytes, bool success, int error) {
    // 进回调先抓一个强引用：保证整个回调期间对象不会被析构。
    // 因为 drop_io_ref() 有可能放掉最后一个引用。
    std::shared_ptr<Session> guard = shared_from_this();
    (void)guard;

    auto* ctx = reinterpret_cast<IoContext*>(ov);

    if (ctx->op == Op::Recv) {
        if (!success) {
            drop_io_ref();
            handle_io_error("WSARecv 完成失败", error);
            return;
        }
        handle_recv(bytes);     // 里面可能重新投递 recv（pending_io_ 又 +1）
        drop_io_ref();          // ← 必须在 handle_recv 之后！
        return;
    }

    // Op::Send
    if (!success) {
        drop_io_ref();
        handle_io_error("WSASend 完成失败", error);
        return;
    }
    handle_send(bytes);         // 里面可能重新投递 send
    drop_io_ref();
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
        return false;      // 关闸了，别投
    }
    if (pending_io_ == 0) {
        self_keepalive_ = shared_from_this();   // 从 0 变 1，抓住自己
    }
    ++pending_io_;
    return true;
}

void Session::drop_io_ref() {
    bool shutdown_now = false;
    std::shared_ptr<Session> keepalive_holder;   // 保证 shutdown_socket 期间对象不死
    {
        std::lock_guard<std::mutex> lk(life_mutex_);
        if (pending_io_ > 0) {
            --pending_io_;
        }
        if (pending_io_ == 0 && closed_.load(std::memory_order_acquire)) {
            shutdown_now = true;
            keepalive_holder = std::move(self_keepalive_);   // 放掉自引用，但不在这里析构
        }
    }
    if (shutdown_now) {
        shutdown_socket();
    }
    // keepalive_holder 在这里析构。如果它和外部引用都清了，Session 在这里真正销毁。
}

void Session::do_recv() {
    if (!add_io_ref()) {        // 已关闸就不投了
        return;
    }

    // 保证从 write_ptr() 起至少有 kRecvChunk 字节【连续】可写。
    // 必须在投递之前调，而且调完之后指针不能再动（compact/扩容会搬内存）。
    recv_buf_.ensure_writable(kRecvChunk);

    recv_ctx_.ov = OVERLAPPED{};
    recv_ctx_.op = Op::Recv;
    recv_ctx_.wsa.buf = reinterpret_cast<char*>(recv_buf_.write_ptr());
    recv_ctx_.wsa.len = static_cast<ULONG>(kRecvChunk);

    DWORD flags = 0;
    DWORD received = 0;
    const int rc = ::WSARecv(sock_, &recv_ctx_.wsa, 1, &received, &flags, &recv_ctx_.ov, nullptr);
    if (rc == SOCKET_ERROR) {
        const int err = ::WSAGetLastError();
        if (err != WSA_IO_PENDING) {
            drop_io_ref();
            handle_io_error("WSARecv", err);
        }
    }
}

void Session::handle_recv(std::uint32_t bytes) {
    touch();                       // ① 刷新活跃时间（心跳用）

    if (bytes == 0) {
        // 对端发了 FIN：他不会再发数据了
        close_when_drained();
        return;
    }

    recv_buf_.commit_write(bytes); // ② 承认"内核帮我写了这么多字节"

    decoded_.clear();
    std::string err;
    const std::size_t n = decode_all(recv_buf_, decoded_, &err);  // ③ 解包
    if (n == kDecodeError) {
        std::printf("[session %llu] 非法包: %s\n",
                    static_cast<unsigned long long>(id_), err.c_str());
        close();
        return;
    }

    for (Packet& pkt : decoded_) {
        if (on_packet_) {
            on_packet_(shared_from_this(), std::move(pkt));   // ④ 交给业务
        }
        if (closed_.load(std::memory_order_acquire)) {
            return;     // 业务可能在里面调了 close()，别继续了
        }
    }
    decoded_.clear();

    do_recv();                     // ⑤ 再投递下一次接收
}

void Session::kick_send() {
    std::weak_ptr<Session> weak = weak_from_this();
    io_.post([weak] {
        auto self = weak.lock();       // 对象可能已经被关了
        if (!self) {
            return;
        }
        self->do_send();
    });
}

void Session::do_send() {
    int error = 0;
    {
        std::lock_guard<std::mutex> lk(send_mutex_);
        send_kicked_ = false;

        if (send_posted_ || send_queue_.empty()) {
            return;
        }
        if (closed_.load(std::memory_order_acquire)) {
            return;
        }

        SendChunk& front = send_queue_.front();
        send_ctx_.ov = OVERLAPPED{};
        send_ctx_.op = Op::Send;
        // front.data 在 deque 里是稳定的：push_back 不会让已有元素的地址失效
        send_ctx_.wsa.buf = reinterpret_cast<char*>(front.data.data() + front.sent);
        send_ctx_.wsa.len = static_cast<ULONG>(front.data.size() - front.sent);

        if (!add_io_ref()) {
            return;                  // 关闸了
        }
        send_posted_ = true;

        // WSASend 不会阻塞：要么立刻完成，要么返回 WSA_IO_PENDING。
        // 所以这里持着锁调它是安全的，而且这样才能和 close() 的"关闸"动作互斥。
        DWORD sent = 0;
        const int rc = ::WSASend(sock_, &send_ctx_.wsa, 1, &sent, 0, &send_ctx_.ov, nullptr);
        if (rc == SOCKET_ERROR) {
            error = ::WSAGetLastError();
            if (error != WSA_IO_PENDING) {
                send_posted_ = false;
            } else {
                error = 0;
            }
        }
    }   // ← 锁在这里释放

    if (error != 0) {
        drop_io_ref();
        handle_io_error("WSASend", error);
    }
}

void Session::handle_send(std::uint32_t bytes) {
    bool kick = false;
    bool close_now = false;
    {
        std::lock_guard<std::mutex> lk(send_mutex_);
        send_posted_ = false;

        if (!send_queue_.empty()) {
            SendChunk& front = send_queue_.front();
            front.sent += bytes;                 // ← 部分写：只推进游标
            if (front.sent >= front.data.size()) {
                send_queue_.pop_front();         // ← 这个 chunk 才算发完
            }
        }

        if (!send_queue_.empty()) {
            if (!send_kicked_) {
                send_kicked_ = true;
                kick = true;                     // 还有剩下的（可能是半截的 front），继续发
            }
        } else if (drain_then_close_) {
            close_now = true;                    // 队列空了，而且之前答应了"发完就关"
        }
    }
    if (kick) {
        kick_send();
    }
    if (close_now) {
        close();
    }
}

void Session::handle_io_error(const char* what, int error) {
    // ERROR_OPERATION_ABORTED(995) 是我们自己 CancelIoEx 导致的，属于正常路径
    if (error != ERROR_OPERATION_ABORTED && !closed_.load(std::memory_order_acquire)) {
        std::printf("[session %llu] %s, err=%d (%s)\n",
                    static_cast<unsigned long long>(id_), what, error, peer_text_.c_str());
    }
    close();
}

void Session::shutdown_socket() {
    if (shutdown_done_.exchange(true)) {
        return;
    }
    if (sock_ != INVALID_SOCKET) {
        const SOCKET s = sock_;
        sock_ = INVALID_SOCKET;
        ::shutdown(s, SD_BOTH);    // 礼貌地发个 FIN，尽力而为
        ::closesocket(s);
    }

    if (on_close_) {
        CloseCallback cb = std::move(on_close_);
        on_close_ = nullptr;       // 保证只回调一次
        cb(shared_from_this());    // 业务层在这里把自己从 SessionManager 里摘掉
    }
}

}  // namespace arena::net
