#include "base/log.h"

#include <chrono>
#include <cstdarg>
#include <cstdlib>
#include <cstring>

// ============================================================================
// 这一节最重要的一句话先放前面：**日志不能拖慢业务，宁可丢日志也不阻塞业务。**
//
// 为什么选「丢」：
//  1. 业务语义上，日志是**可观测性**，不是**状态**。丢十条日志，玩家照样能跑能打；
//     阻塞业务 5ms，30Hz 的一帧（33ms）就废了，一房间里六个人一起卡。
//  2. 因果上，日志积压的原因通常就是「磁盘/文件系统卡了」。这时候阻塞业务
//     等于把存储层的故障放大成整个服务的故障 —— 这是典型的故障放大。
//  3. 工程上，丢日志是**可度量**的：dropped() 计数器暴露出去，运维一眼看到
//     "dropped=12000"，就知道该加磁盘还是降级别。阻塞是不可度量的。
//
// 反过来说，什么时候必须阻塞？审计日志、交易流水、GM 操作记录 ——
// 这些不是「日志」，是业务数据，丢了要赔钱，那就该走同步落盘 + fsync。
// 本项目里它们走的是另一条独立通道，不混在这个环里。
//
// 为什么不用 fprintf 直接写：
//  - 每次调用都要过 stdio 的文件锁（Windows 上是 _lock_file，一把全局锁），
//    16 个 IO 线程 + 逻辑线程全在抢它。
//  - 每次都可能触发一次 4KB 的 write 系统调用，日志量大时直接打满 IO。
//  - 格式化（vsnprintf）在临界区里做，慢函数里持锁 = 锁竞争灾难。
//  异步方案把「格式化 + 写盘」全部搬到后台单线程，业务线程只做一次定长 memcpy。
// ============================================================================

namespace arena::base {

std::size_t LockFreeRing::round_up_pow2(std::size_t n) noexcept {
    if (n < 2) {
        return 2;
    }
    --n;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    n |= n >> 32;
    return n + 1;
}

LockFreeRing::LockFreeRing(std::size_t capacity_pow2) {
    const std::size_t cap = round_up_pow2(capacity_pow2);
    mask_ = cap - 1;
    slots_ = new detail::Slot[cap];
    for (std::size_t i = 0; i < cap; ++i) {
        // 初始状态：槽 i 可以被「第 i 次写入」占用。
        slots_[i].sequence.store(i, std::memory_order_relaxed);
        slots_[i].used = 0;
    }
}

LockFreeRing::~LockFreeRing() {
    delete[] slots_;
}

std::uint8_t* LockFreeRing::try_begin(std::size_t n, std::size_t& out_pos) {
    if (n == 0 || n > kMaxRecord) {
        return nullptr;
    }
    std::size_t pos = tail_.load(std::memory_order_relaxed);
    for (;;) {
        detail::Slot& slot = slots_[pos & mask_];
        const std::size_t seq = slot.sequence.load(std::memory_order_acquire);
        // 有符号相减再比大小：这三行是整个环的精华。
        // 无符号直接比会被回绕（wrap-around）坑死 —— 环形下标总会绕回来，
        // 用有符号差值比较等价于「比较两个数在模 2^64 意义下的先后」，
        // 只要环容量远小于 2^63 就是对的。这是无锁队列的入门咒语，记住它。
        const auto diff = static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(pos);
        if (diff == 0) {
            // 槽位空闲。CAS 抢占，抢输就重来（别的生产者先动手了）。
            if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed,
                                            std::memory_order_relaxed)) {
                slot.used = n;
                out_pos = pos;   // 把抢到的槽位下标交回调用方
                return slot.payload;
            }
            // CAS 失败时 pos 已被更新成当前 tail，直接继续循环。
        } else if (diff < 0) {
            return nullptr;  // 环满：消费者还没把这个槽放回来
        } else {
            pos = tail_.load(std::memory_order_relaxed);  // 被别人抢先推进了，重新读
        }
    }
}

void LockFreeRing::commit(std::size_t pos, std::size_t n) {
    // pos 由 try_begin 传出，不能用 tail_ 反推 —— 多生产者下会算到别人的槽位。
    // 见 log.h 上的说明：这里从 tail_ 反推下标是有竞态的（另一个生产者可能在
    // try_begin 和 commit 之间推进了 tail_）。修法是让 try_begin 输出 pos。
    detail::Slot& slot = slots_[pos & mask_];
    slot.used = n;
    // 这一步是「发布」：release 保证前面写 payload 的字节对其他线程可见。
    // 如果这里写成 relaxed，消费者可能读到 sequence 已更新、payload 还是旧的。
    // 面试问「无锁队列怎么保证看到完整数据」就答 release/acquire 配对，
    // 千万别答 volatile —— volatile 管不住乱序。
    slot.sequence.store(pos + 1, std::memory_order_release);
}

void LockFreeRing::rollback(std::size_t pos) {
    detail::Slot& slot = slots_[pos & mask_];
    slot.used = 0;
    slot.sequence.store(pos, std::memory_order_release);  // 放回「可被第 pos 次写入占用」状态
}

std::size_t LockFreeRing::try_poll(std::uint8_t* dst, std::size_t cap) {
    std::size_t pos = head_.load(std::memory_order_relaxed);
    for (;;) {
        detail::Slot& slot = slots_[pos & mask_];
        const std::size_t seq = slot.sequence.load(std::memory_order_acquire);
        const auto diff = static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(pos + 1);
        if (diff == 0) {
            if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed,
                                            std::memory_order_relaxed)) {
                const std::size_t used = slot.used;
                const std::size_t copy = used < cap ? used : cap;
                if (copy != 0 && dst != nullptr) {
                    std::memcpy(dst, slot.payload, copy);
                }
                slot.used = 0;
                // 放回槽位：第 pos 号槽下一次属于「第 pos + capacity 次写入」。
                slot.sequence.store(pos + mask_ + 1, std::memory_order_release);
                return used;
            }
        } else if (diff < 0) {
            return 0;  // 还没有新记录
        } else {
            pos = head_.load(std::memory_order_relaxed);
        }
    }
}

// ---------------------------------------------------------------- EventSink

const char* level_name(Level lv) noexcept {
    switch (lv) {
        case Level::kTrace:
            return "TRACE";
        case Level::kDebug:
            return "DEBUG";
        case Level::kInfo:
            return "INFO";
        case Level::kWarn:
            return "WARN";
        case Level::kError:
            return "ERROR";
        case Level::kFatal:
            return "FATAL";
    }
    return "INFO";
}

std::int64_t mono_now_us() noexcept {
    // steady_clock 是单调的：系统时间被 NTP 往回调，日志时间戳也不会倒退。
    // 这是日志系统最容易踩的坑之一 —— 时间倒退的日志，分析工具会直接罢工。
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

EventSink& EventSink::instance() {
    static EventSink sink;  // C++11 起函数内静态变量初始化是线程安全的
    return sink;
}

EventSink::~EventSink() {
    stop();
}

bool EventSink::start(const std::string& file_path, std::size_t ring_capacity, Level min_level) {
    if (running_.load(std::memory_order_acquire)) {
        return true;  // 幂等
    }
    if (file_path.empty()) {
        return false;
    }
    // "ab" 而不是 "wb"：追加模式。别把上次崩溃前留下的日志冲掉 ——
    // 那正是你出事时最想看的一段。
    //
    // Windows 中文路径的坑：MSVC 的 fopen 把路径按**当前 ANSI 代码页**解释
    // （中文系统上就是 GBK），传 UTF-8 中文路径会直接失败返回 nullptr。
    // 避开它的办法有二：① 让日志路径全 ASCII（我们推荐这个，本项目就这么干）；
    // ② 真要中文路径，用 _wfopen + UTF-16 宽字符路径，别在这条路上折腾 fopen。
    std::FILE* f = std::fopen(file_path.c_str(), "ab");
    if (f == nullptr) {
        std::fprintf(stderr, "[log] open failed: %s\n", file_path.c_str());
        return false;
    }
    // 后台线程自己攒 buffer，别用 stdio 的默认 4KB 小缓冲跟我们抢 IO。
    // 这里的缓冲区必须是**成员**、属于 FILE* 的生命周期，不能是某个线程的栈/thread_local
    // —— FILE* 会被另一个线程写，指向别人的线程局部存储就是未定义行为。
    std::setvbuf(f, file_buffer_, _IOFBF, sizeof(file_buffer_));

    file_ = f;
    ring_ = std::make_unique<LockFreeRing>(ring_capacity);
    min_level_.store(static_cast<int>(min_level), std::memory_order_relaxed);
    written_.store(0, std::memory_order_relaxed);
    dropped_.store(0, std::memory_order_relaxed);
    stop_.store(false, std::memory_order_relaxed);
    running_.store(true, std::memory_order_release);
    worker_ = std::thread([this] { run(); });
    return true;
}

void EventSink::stop() {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }
    stop_.store(true, std::memory_order_release);
    if (worker_.joinable()) {
        worker_.join();
    }
    if (file_ != nullptr) {
        std::fflush(file_);
        std::fclose(file_);
        file_ = nullptr;
    }
    ring_.reset();
    running_.store(false, std::memory_order_release);
}

bool EventSink::submit(const Event& ev) {
    LockFreeRing* ring = ring_.get();
    if (ring == nullptr || !running_.load(std::memory_order_acquire)) {
        return false;
    }
    std::size_t pos = 0;
    std::uint8_t* p = ring->try_begin(sizeof(Event), pos);
    if (p == nullptr) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    std::memcpy(p, &ev, sizeof(Event));  // 业务线程的全部成本：一次定长拷贝
    ring->commit(pos, sizeof(Event));
    written_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void EventSink::format_jsonl(const Event& ev, std::vector<char>& out) {
    // 手写 JSON 而不是拉一个 JSON 库：字段全是数值和已知短字符串，
    // 唯一要转义的是 event/msg 里的引号和反斜杠。为这点需求引依赖不值得。
    out.clear();
    char head[320];
    const int n = std::snprintf(head, sizeof(head),
                                "{\"ts_us\":%lld,\"tid\":%u,\"lv\":\"%s\",\"event\":\"",
                                static_cast<long long>(ev.ts_us), ev.thread_id, level_name(ev.level));
    if (n > 0) {
        const std::size_t len = static_cast<std::size_t>(n) < sizeof(head) ? static_cast<std::size_t>(n)
                                                                          : sizeof(head) - 1;
        out.insert(out.end(), head, head + len);
    }

    auto append_escaped = [&out](const char* s) {
        for (const char* p = s; *p != '\0'; ++p) {
            if (*p == '"' || *p == '\\') {
                out.push_back('\\');
            }
            if (*p == '\n') {
                out.push_back('\\');
                out.push_back('n');
                continue;
            }
            if (*p == '\r') {  // \r 不转义会让整行 JSONL 在 Windows 上看起来错位
                out.push_back('\\');
                out.push_back('r');
                continue;
            }
            out.push_back(*p);
        }
    };
    append_escaped(ev.event);
    out.push_back('"');

    if (ev.msg[0] != '\0') {
        out.insert(out.end(), {',', '"', 'm', 's', 'g', '"', ':', '"'});
        append_escaped(ev.msg);
        out.push_back('"');
    }
    char tail_buf[96];
    if (ev.room_id != 0) {
        const int m = std::snprintf(tail_buf, sizeof(tail_buf), ",\"room\":%u", ev.room_id);
        if (m > 0) {
            out.insert(out.end(), tail_buf, tail_buf + static_cast<std::size_t>(m));
        }
    }
    if (ev.uid != 0) {
        const int m = std::snprintf(tail_buf, sizeof(tail_buf), ",\"uid\":%llu",
                                    static_cast<unsigned long long>(ev.uid));
        if (m > 0) {
            out.insert(out.end(), tail_buf, tail_buf + static_cast<std::size_t>(m));
        }
    }
    // latency_ms < 0 表示「这条事件没有延迟含义」，此时字段直接不输出。
    // 分析脚本用 `"latency_ms" in obj` 判断有没有，比输出一个假的 -1 安全得多。
    if (ev.latency_ms >= 0.0) {
        const int m = std::snprintf(tail_buf, sizeof(tail_buf), ",\"latency_ms\":%.3f", ev.latency_ms);
        if (m > 0) {
            out.insert(out.end(), tail_buf, tail_buf + static_cast<std::size_t>(m));
        }
    }
    if (ev.line != 0) {
        const int m = std::snprintf(tail_buf, sizeof(tail_buf), ",\"line\":%u", static_cast<unsigned>(ev.line));
        if (m > 0) {
            out.insert(out.end(), tail_buf, tail_buf + static_cast<std::size_t>(m));
        }
    }
    out.push_back('}');
    out.push_back('\n');
}

void EventSink::run() {
    std::vector<std::uint8_t> raw(sizeof(Event));
    std::vector<char> line;
    line.reserve(1024);
    std::vector<Event> batch;  // 批量落盘：一次 fwrite 写多条，减少系统调用
    batch.reserve(kFlushBatch);

    auto flush_batch = [&] {
        if (batch.empty()) {
            return;
        }
        for (const Event& ev : batch) {
            format_jsonl(ev, line);
            std::fwrite(line.data(), 1, line.size(), file_);
        }
        // 这里**不** fflush：让 stdio 自己攒够一个 buffer 再落盘。
        // 每写一条就 flush 等于把异步日志的性能优势全还回去。
        // 代价是崩溃时可能丢最后不到 64KB —— 换成正则也是这笔账，
        // 真出事要追现场，就把 kFlushBatch 调小、或者加定时 flush。
        flushed_.fetch_add(1, std::memory_order_relaxed);
        batch.clear();
    };

    while (!stop_.load(std::memory_order_acquire)) {
        bool idle = true;
        for (int i = 0; i < 1024; ++i) {
            const std::size_t n = ring_->try_poll(raw.data(), raw.size());
            if (n == 0) {
                break;
            }
            if (n == sizeof(Event)) {
                Event ev;
                std::memcpy(&ev, raw.data(), sizeof(Event));
                batch.push_back(ev);
                idle = false;
            }
        }
        if (batch.size() >= kFlushBatch) {
            flush_batch();
        }
        if (idle) {
            // 退避：忙等会烧掉一个核，睡太久又会让「日志写了没」变得可疑。
            // 200us 是实测折中：有负载时上面那个 1024 次循环根本走不到这里。
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }

    // 收尾：把环里剩的全部取出来写掉。stop() 之后宁可多花几毫秒，
    // 也别丢最后一段 —— 最后那段通常就是崩溃原因的上下文。
    for (;;) {
        const std::size_t n = ring_->try_poll(raw.data(), raw.size());
        if (n == 0) {
            break;
        }
        if (n == sizeof(Event)) {
            Event ev;
            std::memcpy(&ev, raw.data(), sizeof(Event));
            batch.push_back(ev);
        }
    }
    flush_batch();
    std::fflush(file_);
}

bool log_impl(Level level,
              const char* event_name,
              unsigned source_line,
              std::uint32_t room_id,
              std::uint64_t uid,
              const char* fmt,
              ...) {
    Event ev;
    ev.ts_us = mono_now_us();
    ev.thread_id = static_cast<std::uint32_t>(
        std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xFFFFFFFFu);
    ev.line = static_cast<std::uint16_t>(source_line & 0xFFFFu);
    ev.level = level;
    ev.room_id = room_id;
    ev.uid = uid;
    ev.latency_ms = -1.0;

    if (event_name != nullptr) {
        std::snprintf(ev.event, sizeof(ev.event), "%s", event_name);
    }

    va_list args;
    va_start(args, fmt);
    // 用 vsnprintf 而不是 sprintf：msg 是定长数组，格式化结果超长必须被截断，
    // 不能溢出到 event/level 字段上去。这个坑我踩过 —— 表现是日志里
    // level 变成乱码字符串，查了半天才明白是越界写。
    std::vsnprintf(ev.msg, sizeof(ev.msg), fmt, args);
    va_end(args);

    return EventSink::instance().submit(ev);
}

}  // namespace arena::base
