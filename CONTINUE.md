# ArenaCore · 续做交接文档

> 最后更新：2026-10-03（D11，demo 可玩）
> 这个文件是「下次继续做」的入口。读完它你就知道从哪开始，不用手足无措。

## 1. 现在做到哪了（一句话）

**网络层 + 逻辑层完整落地，demo 能跑完整 3v3 对局**（真实 TCP 连接、30Hz 逻辑帧、战斗结算、AOI 增量快照广播）。

## 2. 怎么跑起来（1 分钟）

```bat
双击 E:\projects\ArenaCore\run_demo.cmd
```

它会：起服务器(9527) → 预填 4 bot → 连 2 个 bot 客户端 → 满 6 人 3v3 开打 → 服务器窗口滚动广播日志。

其他验证程序（全部在 `build\` 下）：
- `arena_tests.exe`：17 个单测（缓冲/编解码）
- `combat_smoke.exe`：战斗结算 17 断言（射程/CD/反外挂/确定性）
- `aoi_smoke.exe`：AOI 13 断言
- `snapshot_smoke.exe`：状态同步（紧凑编码 40%）
- `game_server.exe`：集成服务器（run_demo 用的）
- `bot_client.exe`：演示 bot

## 3. 代码地图

```
src/base/     buffer.h 环形缓冲 · log.* 无锁日志 · alloc_hook.h 分配计数
src/net/      io_service.h 接口 · iocp_service.* 事件循环 · acceptor.* 监听
              session.* 连接生命周期 · session_manager.* 连接表 · codec.* 协议编解码
src/concurrency/ object_pool.h 对象池
src/game/     world.h 定点数/Player · room.h/.cpp 七阶段逻辑帧 · aoi.* 九宫格
              seq.h 回绕序号 · snapshot.h 快照 · snapshot_codec.* 紧凑编解码
tools/        *_smoke.cpp 各种验证 · game_server_smoke.cpp 集成服务器
              bot_client.cpp 演示 bot
CMakeLists.txt 构建（Ninja + MSVC，/W4 零警告）
```

## 4. 下一步做什么（按优先级）

### ① 房间管理 + 匹配（教程第 6 章 §3）
- 现状：只有 1 个 Room，add_player 满 6 人开打
- 要做：RoomManager（房间表 + 状态机 kWaiting/kFighting/kSettled/kClosed + 空闲回收）、匹配逻辑（一致性哈希分片）
- 关键文件：新建 `src/game/room_manager.h/.cpp`

### ② Redis + 一致性哈希 + 压测（教程第 6 章）
- 自写 Redis 客户端（RESP 协议，零依赖）→ zset 排行榜
- 一致性哈希 + 虚拟节点
- 压测工具（把 bot_client 扩展成多连接压测）
- 关键文件：新建 `src/store/redis_client.h/.cpp`、`src/match/consistent_hash.h`

### ③ client-demo 可视化前端（可选加分）
- 现状：命令行 bot。可接一个简单 WebSocket/前端展示对局
- 边界：客户端非自研，面试时主动声明

### ④ GitHub 仓库整理
- README 已写，进度看板在 `国庆学习\01-工程-demo\docs\`

## 5. 关键约定（面试/继续开发都要守住）

- **服务端权威**：客户端只传意图（skill_id+target_id+seq），伤害/射程/位移服务端算
- **确定性**：定点整数（kFpOne=65536）不用浮点；帧计数 CD 不用秒
- **所有性能数字必须实测**，不编造；AOI 降幅带场景（6 人房≈0、40 人房 69-95%）
- **单机模拟分布式**，不是真集群；客户端非自研，主动声明
- 每次改动：编译零警告 + 跑对应 smoke 验证再提交

## 6. 常见坑（踩过的）

- `setvbuf(stdout, nullptr, _IOLBF, 0)` 在 MSVC 下**崩溃 0xC0000409** → 用 `_IONBF`
- stdout 重定向文件是块缓冲，强杀进程丢日志 → 用 stderr 或 _IONBF
- `to_cell` 负数要 floor 语义（`-1` 落格 -1，不是 0）
- OVERLAPPED 必须常驻 Session 成员，不能栈上
- 房间满 6 人才 kFighting 才广播（测试要凑满人或手动改）

## 7. 构建命令

```bat
call E:\DevTools\vcenv.bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
```
