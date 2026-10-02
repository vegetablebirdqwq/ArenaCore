# OpsAgent（AI 运营支撑链路）

游戏服务器的"运营支撑"：服务器出故障时，靠人肉翻几万行日志定位。OpsAgent 把这件事自动化：

```
日志/指标流 → ① 规则检测（纯统计，确定性）
            → ② 上下文组装（日志压缩成证据）
            → ③ LLM 根因分析（调外部 API，模型不是自研）
            → ④ 处置草案（动作白名单，必须人点头才执行）
```

## 已落地

### 第一层规则检测（ops/analyzers.py）

**不依赖任何第三方库，纯 Python 标准库。** 告警绝不允许 LLM 参与——否则"系统到底有没有出事"都不可复现。

8 条规则（R1-R8）：

| 规则 | 检测什么 | 机制 |
|---|---|---|
| R1_tick_stall | 逻辑帧 P99 超预算 | 固定阈值 8ms warn / 15ms crit |
| R2_error_spike | 错误率飙升 | 阈值 1% warn / 5% crit |
| R3_disconnect_storm | 掉线风暴 | 阈值 2% warn / 10% crit |
| R4_redis_down/slow | Redis 挂了/变慢 | 健康检查 + P99 5ms/20ms |
| R5_match_backlog | 匹配队列积压 | 阈值 200/1000 |
| R6_conns_drop | 连接数断崖 | 环比掉 20% |
| R7_mem_rising | 内存持续增长（泄漏） | 连续 10 窗口递增 |
| R8_mutation_* | 相对自己历史 3σ 突变 | EWMA + 3σ |

**三个设计点（面试讲）**：
1. **去抖 + 冷却**：连续 3 个窗口超阈才报（单点抖动不报），同一规则 60 秒内只报一次
2. **固定阈值 vs EWMA 突变**：阈值抓"已知的坑"，EWMA+3σ 抓"行为悄悄变了"（内存泄漏早期）
3. **确定性**：同样的输入永远同样的输出，可注入故障验证

## 验证（ops/test_analyzers.py）

注入已知故障 → 断言对应规则触发（教程 6.7 的硬要求）：
- 帧卡顿 → R1 ✓
- Redis 慢 → R4 ✓
- 掉线 5% → R3 ✓
- 队列积压 → R5 ✓
- 连接数环比掉 20% → R6 ✓
- 内存连续上升 → R7 ✓
- 正常波动 60 窗口 → 0 误报 ✓

```
python ops/test_analyzers.py
```

## 完整闭环：故障注入 + 命中率（ops/inject_fault.py + eval_faults.py）

**"OpsAgent 能定位根因"这句话本身是不可验证的。** 先注入**已知**故障、把期望根因写成 ground_truth.json，再让链路跑，得出可复现的命中率。

```
python ops/inject_fault.py --out ops_state/scenarios   # 生成 5 种故障场景 + ground truth
python ops/eval_faults.py --scenarios ops_state/scenarios
```

**实测结果：5 种故障命中率 5/5 = 100%**（纯规则降级路径，不依赖 LLM API）：

| 场景 | 注入故障 | 期望根因 | 实际判定 | 结果 |
|---|---|---|---|---|
| F01 | 逻辑帧卡顿 | logic_tick_stall | logic_tick_stall | 命中 |
| F02 | 掉线风暴 | client_disconnect_storm | client_disconnect_storm | 命中 |
| F03 | Redis 不可用 | redis_unavailable | redis_unavailable | 命中 |
| F04 | 房间对象泄漏 | room_leak | room_leak | 命中 |
| F05 | 匹配队列积压 | match_queue_backlog | match_queue_backlog | 命中 |

**评估标准（防止自欺欺人）**：故障窗内**第一条 critical 告警**的根因与期望一致才算命中（不是"任意一条命中"）。

## 待做（教程 6.7 后半）

- [x] ~~第一层规则检测（R1-R8）~~
- [x] ~~日志管道（JSONL 解析 + 事件聚合 + 切片摘要）~~
- [x] ~~故障注入 + 命中率评估（5/5 可复现）~~
- [x] ~~纯规则降级分析器（离线可用）~~
- [ ] LLM 根因分析（接外部 API；**模型不是自研**，面试先声明）
- [ ] JSON schema 约束 + 校验（防模型输出多余字段）
- [ ] 动作白名单 + 人确认执行（防幻觉出 rm -rf 级别动作）
- [ ] 超时/重试/降级/熔断/成本记账
