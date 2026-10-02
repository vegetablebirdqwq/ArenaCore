# ArenaCore · 续做交接文档

> 最后更新：2026-10-03（D15，压测完成）
> 这个文件是「下次继续做」的入口。读完它你就知道从哪开始，不用手足无措。

## 1. 现在做到哪了（一句话）

**网络层 + 逻辑层 + 存储 + 压测全部落地**：真实 TCP 连接、30Hz 逻辑帧、战斗结算、AOI 增量快照、自写 Redis 客户端、一致性哈希、定宽直方图、压测 Bot + 压测报告。GitHub 已推送。

## 2. 怎么跑起来（1 分钟）

```bat
双击 E:\projects\ArenaCore\run_demo.cmd    ← 起服务器 + 2 bot 打一局
```

其他验证程序（全部在 `build\` 下）：
- `arena_tests.exe`：单测（20 用例：缓冲/编解码/一致性哈希）
- `ring_lab.exe`：一致性哈希实验（取模 80% vs 一致哈希 20% 迁移）
- `redis_demo.exe 127.0.0.1 6399`：自写 RESP 客户端演示（先起 D:\redis）
- `hist_lab.exe`：直方图 + 伪共享实验
- `bench_bot.exe --conns 300 --duration 12`：压测（先起 game_server）
- `game_server.exe`：集成服务器

## 3. 代码地图

```
src/base/     buffer.h 环形缓冲 · log.* 无锁日志 · alloc_hook.h 分配计数
              resp_client.h/.cpp 自写 Redis 客户端(RESP) · histogram.h 定宽直方图
              consistent_hash.h 一致性哈希环
src/net/      io_service.h · iocp_service.* · acceptor.* · session.*
              session_manager.* · codec.* · protocol_cmds.h（协议常量，压测共用）
src/game/     world.h 定点数/Player · room.h/.cpp 七阶段逻辑帧 · aoi.* 九宫格
              seq.h · snapshot.h · snapshot_codec.*
tools/        *_smoke.cpp 各种验证 · ring_lab · redis_demo · hist_lab
              bench_bot.cpp 压测 · game_server_smoke.cpp 集成服务器 · bot_client.cpp
docs/         压测报告.md
```

## 4. 下一步做什么（按优先级）

### ① RoomManager + 多房间分片（**修复压测发现的瓶颈**）
- 现状：只有 1 个 Room，所有连接塞进去，广播 O(N²)
- **压测已证明**：单房间 500 连接崩溃（D15 报告），300 连接就是拐点
- 要做：RoomManager（房间表 + 状态机 + 空闲回收）+ 匹配分片（一致性哈希），压测 Bot 改为分布到多房间
- 关键文件：新建 `src/game/room_manager.h/.cpp`

### ② OpsAgent（AI 运营支撑，教程 6.7，Python）
- 日志聚合 → LLM 告警根因 → 动作白名单执行
- 这是简历里「AI 运营支撑链路」那条线的实现

### ③ client-demo 可视化前端（可选加分）
- 边界：客户端非自研，面试时主动声明

## 5. 关键约定（面试/继续开发都要守住）

- **服务端权威**：客户端只传意图，伤害/射程/位移服务端算
- **确定性**：定点整数（kFpOne=65536）不用浮点；帧计数 CD 不用秒
- **所有性能数字必须实测**，不编造
- **压测 Bot 复用服务器 codec**（protocol_cmds.h 单一来源），不能另写协议
- **单机模拟分布式**，不是真集群；客户端非自研，主动声明
- 每次改动：编译零警告 + 跑对应验证再提交

## 6. 常见坑（踩过的）

- `setvbuf(stdout, nullptr, _IOLBF, 0)` 在 MSVC 下崩溃 0xC0000409 → 用 `_IONBF`
- `.bat/.cmd 必须纯 ASCII`（cmd 无法解析 UTF-8 中文，会把中文行拆碎报错）
- 程序输出中文要 `SetConsoleOutputCP(CP_UTF8)`，否则 GBK 控制台乱码
- winsock2.h 必须第一个 include（resp_client.cpp 的坑）
- `to_cell` 负数要 floor 语义
- `LatencyHistogram` 含 std::atomic 不可拷贝/移动，传引用或用 unique_ptr
- Start-Process 的 ArgumentList 不接受空字符串参数（起 Redis 用 cmd start）

## 7. 构建命令

```bat
call E:\DevTools\vcenv.bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
```

## 8. 本机 Redis

```bat
cmd /c 'start "" /min D:\redis\redis-server.exe --port 6399'
redis_demo.exe 127.0.0.1 6399
```
