#pragma once

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "base/buffer.h"
#include "net/codec.h"
#include "net/iocp_service.h"

namespace arena::net {

/// 一条客户端连接。
///
/// 生命周期约定：
///   · SessionManager 持有一个 shared_ptr；
///   · Session 自己在"还有 IO 在飞"的时候持有 self_keepalive_；
///   · 两边都放掉之后对象才析构 —— 保证不会有完成包打到已经析构的对象上。
class Session final : public CompletionHandler,
                      public std::enable_shared_from_this<Session> {
public:
    using PacketCallback = std::function<void(const std::shared_ptr<Session>&, Packet&&)>;
    using CloseCallback = std::function<void(const std::shared_ptr<Session>&)>;

    static constexpr std::size_t kRecvChunk = 64 * 1024;
    static constexpr std::size_t kRecvCapacity = 128 * 1024;   // 留两倍，让 ensure_writable 走 compact 而不是扩容

    Session(IocpService& io, SOCKET sock, std::uint64_t id, std::string peer_text);
    ~Session() override;

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    /// 开始收数据。必须在设置好回调之后调。
    void start();

    /// 线程安全。往写队列里塞一个已经 encode 好的完整包。
    void send(std::vector<std::uint8_t> packet);

    /// 立刻关（超时踢人、非法包）。
    void close();

    /// 把写队列里的数据发完再关（收到对端 FIN 之后用）。
    void close_when_drained();

    void on_io_completed(OVERLAPPED* ov, std::uint32_t bytes, bool success, int error) override;

    std::uint64_t id() const noexcept { return id_; }
    bool closed() const noexcept { return closed_.load(std::memory_order_acquire); }
    const std::string& peer_text() const noexcept { return peer_text_; }

    /// 刷新活跃时间。收到任何字节都会调。
    void touch() noexcept;
    std::chrono::steady_clock::time_point last_active() const noexcept;

    void set_packet_callback(PacketCallback cb) { on_packet_ = std::move(cb); }
    void set_close_callback(CloseCallback cb) { on_close_ = std::move(cb); }

private:
    enum class Op : std::uint8_t { Recv, Send };

    /// OVERLAPPED 必须是第一个成员 —— 这样 &ctx.ov == &ctx，
    /// 才能从完成包里的 OVERLAPPED* 反推回整个 IoContext。
    struct IoContext {
        OVERLAPPED ov{};
        Op op = Op::Recv;
        WSABUF wsa{};
    };

    struct SendChunk {
        std::vector<std::uint8_t> data;
        std::size_t sent = 0;
    };

    bool add_io_ref();
    void drop_io_ref();

    void do_recv();
    void do_send();
    void kick_send();

    void handle_recv(std::uint32_t bytes);
    void handle_send(std::uint32_t bytes);
    void handle_io_error(const char* what, int error);
    void shutdown_socket();

    IocpService& io_;
    SOCKET sock_ = INVALID_SOCKET;
    std::uint64_t id_ = 0;
    std::string peer_text_;

    // ---- 接收侧：只有 IOCP 线程碰，不需要锁
    base::RingBuffer recv_buf_{kRecvCapacity};
    std::vector<Packet> decoded_;
    IoContext recv_ctx_;

    // ---- 发送侧：可能被逻辑线程碰，需要锁
    IoContext send_ctx_;
    std::mutex send_mutex_;
    std::deque<SendChunk> send_queue_;
    bool send_posted_ = false;      // 真的有一个 WSASend 在飞
    bool send_kicked_ = false;      // 已经安排了 worker 去发，但还没跑
    bool drain_then_close_ = false; // "发完就关"

    // ---- 生命周期
    std::mutex life_mutex_;
    int pending_io_ = 0;
    std::shared_ptr<Session> self_keepalive_;

    std::atomic<bool> closed_{false};
    std::atomic<bool> shutdown_done_{false};
    std::atomic<std::int64_t> last_active_us_{0};

    PacketCallback on_packet_;
    CloseCallback on_close_;
};

}  // namespace arena::net
