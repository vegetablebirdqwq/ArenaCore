# ArenaCore

实时对战游戏服务器（3v3 竞技场）+ AI 运营支撑子系统。

- **语言**：C++20
- **工具链**：MSVC 19.37（VS 2022 17.7）+ CMake 3.26 + Ninja 1.11
- **网络模型**：Windows IOCP（Proactor）
- **依赖**：无。仅标准库 + Winsock/Windows API。

## 构建

```powershell
. E:\DevTools\vcenv.ps1
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
.\build\arena_tests.exe
```

## 目录

| 目录 | 内容 |
|---|---|
| `src/base` | 环形缓冲、结构化日志等基础设施 |
| `src/net` | IOCP 事件层、Session、协议编解码 |
| `src/concurrency` | 无锁队列、线程池、对象池、时间轮 |
| `src/game` | 房间、30Hz 逻辑帧、AOI、战斗结算 |
| `src/match` | 一致性哈希与匹配 |
| `src/store` | Redis 客户端 |
| `tools` | 压测 Bot、协议调试工具 |
| `ai` | OpsAgent 运营支撑（Python） |
| `tests` | 单元测试 |

## 架构

见 `docs/架构图.md`。设计说明见资料库中的 `00-ArenaCore-项目方案书.md`。
