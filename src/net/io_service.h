#pragma once

#include <functional>

namespace arena::net {

/// 事件循环的抽象接口。
///
/// 为什么要有这一层：M1 的验收标准是「能正面对比 epoll Reactor」。
/// 有了这个接口，以后想加一个 Linux 版（epoll + 用户态 Proactor 模拟）
/// 只要再实现一个 EpollService，上层 Session / Acceptor 一行都不用改。
class IoService {
public:
    using Task = std::function<void()>;

    virtual ~IoService() = default;

    /// 起工作线程。
    virtual void start() = 0;
    /// 停工作线程并等待它们退出。
    virtual void stop() = 0;
    /// 把一个闭包丢到事件循环的线程上去执行。线程安全。
    virtual bool post(Task task) = 0;
};

}  // namespace arena::net
