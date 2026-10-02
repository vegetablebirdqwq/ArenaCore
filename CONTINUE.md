# ArenaCore · 续做交接文档

> 最后更新：2026-10-03（D19，OpsAgent 完整闭环）
> 这个文件是「下次继续做」的入口。读完它你就知道从哪开始，不用手足无措。

## 1. 现在做到哪了（一句话）

**项目核心全部落地**：网络层 + 逻辑层 + 存储 + 压测 + 多房间分片 + OpsAgent AI 运营链路。
GitHub 36 提交，进度看板 19/20（95%）。**剩下唯一大项是 client-demo 可视化前端（可选加分）**。

## 2. 怎么跑起来（1 分钟）

```bat
双击 E:\projects\ArenaCore\run_demo.cmd    ← 起服务器 + 2 bot 打一局（多房间）
```

C++ 验证程序（`build\` 下）：
- `arena_tests.exe`：20 单测（缓冲/编解码/一致性哈希）
- `ring_lab.exe`：一致性哈希（取模 80% vs 一致哈希 20%）
- `redis_demo.exe 127.0.0.1 6399`：自写 RESP 客户端（先起 Redis）
- `hist_lab.exe`：直方图 + 伪共享实验
- `bench_bot.exe --conns 500 --duration 12`：压测（多房间 500 连接 0 错误）
- `room_manager_smoke.exe`：多房间分片验证

Python OpsAgent（`ops\` 下）：
- `python ops/test_analyzers.py`：R1-R8 规则检测验证
- `python ops/inject_fault.py && python ops/eval_faults.py`：故障注入命中率 5/5

## 3. 代码地图

```
src/base/     buffer.h · log.* · resp_client.h/.cpp(自写Redis) · histogram.h
              consistent_hash.h(一致性哈希)
src/net/      io_service.h · iocp_service.* · acceptor.* · session.*
              session_manager.* · codec.* · protocol_cmds.h
src/game/     world.h · room.h/.cpp(七阶段) · aoi.* · seq.h · snapshot.*
              room_manager.h/.cpp(多房间分片)
ops/          analyzers.py(规则检测R1-R8+日志管道) · llm_analyzer.py(降级+LLM接口)
              inject_fault.py(5种故障) · eval_faults.py(命中率) · test_analyzers.py
docs/         压测报告.md（单房间 vs 多房间对比）
```

## 4. 下一步做什么

### ① client-demo 可视化前端（唯一剩余大项，可选加分）
- 现状：命令行 bot（收快照打印 hex）
- 可做：一个简单的 HTML/JS 前端连服务器，把快照画成小地图上的点（移动/攻击可视化）
- 边界：客户端非自研，面试主动声明

### ② OpsAgent LLM 真实接入（可选）
- 现状：纯规则降级路径（命中率 5/5），LLMAnalyzer 留了接口
- 接外部 API：实现 `LLMAnalyzer._try_llm()`（组上下文 → urllib 调 API → JSON schema 校验）
- 面试声明：模型是外部 API，自研的是护栏工程

## 5. 关键约定（面试/继续开发都要守住）

- **服务端权威**：客户端只传意图，伤害/射程/位移服务端算
- **确定性**：定点整数不用浮点；帧计数 CD 不用秒
- **所有性能数字必须实测**：压测 500 连接 0 错误（多房间）、OpsAgent 命中率 5/5
- **压测 Bot 复用服务器 codec**（protocol_cmds.h 单一来源）
- **OpsAgent LLM 层是外部 API**，自研的是规则检测/日志管道/护栏
- **单机模拟分布式**，不是真集群；客户端非自研，主动声明
- 每次改动：编译零警告 + 跑对应验证再提交

## 6. 常见坑（踩过的）

- `setvbuf(stdout, nullptr, _IOLBF, 0)` 在 MSVC 下崩溃 0xC0000409 → 用 `_IONBF`
- `.bat/.cmd 必须纯 ASCII`（cmd 无法解析 UTF-8 中文）
- 程序输出中文要 `SetConsoleOutputCP(CP_UTF8)`
- winsock2.h 必须第一个 include
- Room 的 send_ 空函数调用会崩 → 全部调用点判空（D16 修复）
- `LatencyHistogram` 含 atomic 不可拷贝/移动
- ops_state/ 是生成物，已加 .gitignore

## 7. 构建命令

```bat
call E:\DevTools\vcenv.bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
```

## 8. 本机 Redis / OpsAgent

```bat
cmd /c 'start "" /min D:\redis\redis-server.exe --port 6399'
redis_demo.exe 127.0.0.1 6399
python ops/inject_fault.py && python ops/eval_faults.py
```
