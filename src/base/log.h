#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace arena::base {

// 这个警告在我们这儿是**预期**的：alignas(64) 就是故意加填充，让相邻槽位
// 落在不同 cache line 上。/W4 会把每一处都报一遍，直接把警告关掉，
// 否则真正的警告会被这几百行噪音淹掉。
#pragma warning(push)
#pragma warning(disable : 4324)

enum class Level : int {
    kTrace = 0,
    kDebug = 1,
    kInfo = 2,
    kWarn = 3,
    kError = 4,
    kFatal = 5,
};

const char* level_name(Level lv) noexcept;

/// LogHub 落的一条结构化事件。
/// 字段是**定长**的 —— 这是整个异步日志能做无锁的前提：
/// 定长记录才能放进环形槽位里，不需要在锁里做内存分配。
/// event/msg 砍到 48/160 字节是实测的选择：够放 "room_tick_slow"、
/// "player disconnected, reason=heartbeat_timeout" 这类信息，
/// 超长内容应该走「文件行号 + 参数」，而不是把日志当数据库用。
struct Event {
    std::int64_t ts_us = 0;     // 相对进程启动的微秒数，单调时钟，不受系统时间调整影响
    std::uint32_t thread_id = 0;
    std::uint16_t line = 0;
    Level level = Level::kInfo;
    char event[48] = {};
    char msg[160] = {};
    std::uint32_t room_id = 0;  // 0 表示与房间无关
    std::uint64_t uid = 0;      // 0 表示与玩家无关
    double latency_ms = 0.0;    // 负数表示「本次事件不测延迟」，落盘时直接省略该字段
};

namespace detail {

/// 环形槽位。alignas(64) 是为了让相邻槽位的 sequence 落在不同 cache line 上 ——
/// 否则多个生产者 CAS 相邻槽位时会互相把对方的 cache line 打失效（伪共享），
/// 加锁反而更快。这是本项目里最便宜的优化之一。
struct alignas(64) Slot {
    std::atomic<std::uint64_t> sequence;
    std::size_t used = 0;
    alignas(8) std::uint8_t payload[256];
};

static_assert(sizeof(Slot) == 64 * 5, "slot must stay on a 64-byte boundary");

}  // namespace detail

/// 三游标 SPSC 无锁环：一个（或很少几个）生产者拷字节，一个后台线程取整槽。
///
/// 为什么不是「一个 std::mutex 保护的 std::deque<Event>」：
/// 锁本身不是问题，问题是临界区里的**内存分配和析构**。每次 push 一个新 Event
/// 就是一次堆操作，几十万条日志/秒的时候，日志系统自己就成了性能测试对象。
///
/// 为什么生产端要「先占槽、再写、最后 publish」：
/// 消费者只有在 slots_[idx_].sequence == 预期值时才认这个槽有效。
/// 生产者写完 payload 做一次 release 存，消费者 acquire 读 —— 保证 payload
/// 的字节一定先于 sequence 可见。这叫 **release/acquire 配对发布**，
/// 面试问到「无锁队列怎么保证看到完整数据」就答这个，别答 volatile。
class LockFreeRing {
public:
    explicit LockFreeRing(std::size_t capacity_pow2);
    ~LockFreeRing();

    LockFreeRing(const LockFreeRing&) = delete;
    LockFreeRing& operator=(const LockFreeRing&) = delete;

    /// 单次写入上限 = 一个槽位能装的字节数。
    /// 我们的记录只有一种：一个 sizeof(Event) 的事件，所以它天生对得上。
    /// 一旦以后加了变长记录，这里就要重新算，并且要加 static_assert 兜住。
    static constexpr std::size_t kMaxRecord = 256;

    /// 尝试占一个槽位，成功返回可写地址（最多 kMaxRecord 字节）。
    /// 环满或正在被消费时返回 nullptr —— 这里**绝不阻塞**，见 log.cpp 顶部的取舍说明。
    std::uint8_t* try_begin(std::size_t n, std::size_t& out_pos);

    /// 写完了，发布出去。不调这个消费者永远看不到那条记录。
    ///
    /// out_pos 必须一路传回 commit / rollback —— 这是本类唯一的调用约定。
    /// 曾经写错过：commit 里用 tail_.load() - 1 反推下标。单生产者时看不出问题，
    /// 多生产者并发时 try_begin 与 commit 之间 tail_ 可能已被别人推进，
    /// 于是会去 commit【别人的】槽位，日志静默串位。隐蔽，引以为戒。
    /// 教程 01 第 4.4 节讲了完整的推导过程。
    void commit(std::size_t pos, std::size_t n);

    /// 占位后发现写不下，撤销占位。环形队列必须有这条路，否则
    /// 「条件写入」（只有满足条件才落日志）就没法实现。
    void rollback(std::size_t pos);

    /// 取一条记录（消费者侧）。返回拷贝出的字节数，0 表示环空。
    std::size_t try_poll(std::uint8_t* dst, std::size_t cap);

    std::size_t capacity() const noexcept { return mask_ + 1; }

private:
    static std::size_t round_up_pow2(std::size_t n) noexcept;

    detail::Slot* slots_ = nullptr;
    std::size_t mask_ = 0;
    alignas(64) std::atomic<std::size_t> head_{0};  // 消费者位置
    alignas(64) std::atomic<std::size_t> tail_{0};  // 生产者位置
};

#pragma warning(pop)

// ---------------------------------------------------------------- 事件汇聚（单文件）
//
// 简单说明：下面这个 EventSink 是**单文件、单后台线程**版本，够跑通
// 「结构化日志 → JSONL」这条链路，也够压测出「异步 vs 直接 fprintf」的差距。
// 真正按天分文件、按模块分文件、以及多文件轮转，放在 M9 的下一版里做。
class EventSink {
public:
    static EventSink& instance();

    /// 幂等。levels 是长度 6 的数组，索引就是 Level 的整数值。
    bool start(const std::string& file_path, std::size_t ring_capacity, Level min_level);
    void stop();

    bool started() const noexcept { return running_.load(std::memory_order_acquire); }

    bool would_log(Level lv) const noexcept {
        return started() && static_cast<int>(lv) >= min_level_.load(std::memory_order_relaxed);
    }

    /// 投递一条已填好的事件。返回 false 表示环满被丢弃。
    bool submit(const Event& ev);

    /// 已丢弃的记录数 —— 这个数必须暴露出来，否则日志静默丢失是运维事故。
    std::uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }
    /// 累计**接收**的事件数。注意它不是「已经落到磁盘」—— 后面还有 stdio 缓冲。
    std::uint64_t written() const noexcept { return written_.load(std::memory_order_relaxed); }
    /// 后台线程实际提交给 stdio 的批次数。
    std::uint64_t flushed() const noexcept { return flushed_.load(std::memory_order_relaxed); }

    /// 攒够多少条就 fwrite 一批。越小越实时、越费系统调用；越大越省 IO、崩溃时丢得越多。
    static constexpr std::size_t kFlushBatch = 256;
    static constexpr std::size_t kFileBufferSize = 1 << 16;

private:
    EventSink() = default;
    ~EventSink();
    EventSink(const EventSink&) = delete;
    EventSink& operator=(const EventSink&) = delete;

    void run();
    static void format_jsonl(const Event& ev, std::vector<char>& out);

    std::unique_ptr<LockFreeRing> ring_;
    std::FILE* file_ = nullptr;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    std::atomic<int> min_level_{static_cast<int>(Level::kInfo)};
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> written_{0};
    std::atomic<std::uint64_t> flushed_{0};
    // 必须是成员，不能是 thread_local：FILE* 由后台线程写，
    // 缓冲区要是指向「调用 start() 那个线程的线程局部存储」，就是 UB。
    char file_buffer_[kFileBufferSize] = {};
};

// ---------------------------------------------------------------- 打日志的门面

/// 定时基准。进程启动时调一次；只调一次，之后都拿它算相对时间。
std::int64_t mono_now_us() noexcept;

/// 往环里投一条事件。level 不达标就什么都不做（先过滤再格式化，省 CPU）。
/// 第二个参数叫 target 是故意的：event["room_tick_slow"] 这种写法比
/// LOG_INFO("...") 更难写错，也让日志天然带上了「事件名」这个可聚合维度。
#if defined(__GNUC__) || defined(__clang__)
#    define ARENA_PRINTF_LIKE(fmt_index, first_arg) __attribute__((format(printf, fmt_index, first_arg)))
#else
// MSVC 没有 format(printf) 这类属性；开了 /analyze 之后它有自己的检查。
#    define ARENA_PRINTF_LIKE(fmt_index, first_arg)
#endif

bool log_impl(Level level,
              const char* event_name,
              unsigned source_line,
              std::uint32_t room_id,
              std::uint64_t uid,
              const char* fmt,
              ...) ARENA_PRINTF_LIKE(6, 7);

}  // namespace arena::base

/// 用法：
///     ARENA_LOG(Info, "server_start", room_id, uid, "listen_port=%d workers=%d", port, workers);
/// 级别写 Info/Warn/Error（不带 k 前缀），宏自己拼成 Level::kInfo。
#define ARENA_LOG(level, event_name, room, uid, ...)                                              \
    do {                                                                                          \
        if (::arena::base::EventSink::instance().would_log(::arena::base::Level::k##level)) {     \
            ::arena::base::log_impl(::arena::base::Level::k##level, event_name, static_cast<unsigned>(__LINE__), \
                                    room, uid, __VA_ARGS__);                                      \
        }                                                                                         \
    } while (0)

#define ARENA_LOG_SIMPLE(level, target, ...) ARENA_LOG(level, target, 0u, 0ull, __VA_ARGS__)
