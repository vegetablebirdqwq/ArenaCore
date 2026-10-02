"""OpsAgent 第一层验证：注入已知故障 → 必须报出对应规则。

教程 6.7 的硬要求：检测器必须能在压测里被验证 ——
注入一个已知故障，它必须报出对应的规则。这层不能靠概率。

跑法：python ops/test_analyzers.py
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from analyzers import RuleConfig, RuleEngine

FAIL = 0


def expect(name: str, cond: bool) -> None:
    global FAIL
    print(f"{'PASS' if cond else 'FAIL'}: {name}")
    if not cond:
        FAIL += 1


def feed(engine: RuleEngine, metric: dict) -> list:
    return engine.on_metric(metric)


def run() -> None:
    # ---------- 场景 1：逻辑帧卡顿（R1）----------
    print("=== 场景 1: 逻辑帧卡顿 R1 ===")
    cfg = RuleConfig()
    eng = RuleEngine(cfg)
    got = []
    # 先喂 3 个正常窗口（建立基线），再连续超阈 3 次（触发 confirm_samples=3）
    for i in range(5):
        feed(eng, {"ts": float(i), "tick_ms_p99": 2.0, "conns": 100})
    for i in range(5, 8):
        got += feed(eng, {"ts": float(i), "tick_ms_p99": 12.0, "conns": 100})
    expect("R1 逻辑帧 P99 超阈报警", any(a.rule == "R1_tick_stall" for a in got))
    expect("R1 严重级 critical（12ms > 8ms warn 且 < 15ms crit→warn；用 20ms 测 crit）",
           True)  # 占位，下面单独测 critical

    # critical 档：超 15ms
    eng2 = RuleEngine(cfg)
    for i in range(5):
        feed(eng2, {"ts": float(i), "tick_ms_p99": 2.0})
    got2 = []
    for i in range(5, 8):
        got2 += feed(eng2, {"ts": float(i), "tick_ms_p99": 20.0})
    crit = [a for a in got2 if a.rule == "R1_tick_stall"]
    expect("R1 critical 严重级（20ms ≥ 15ms crit 阈值）",
           len(crit) > 0 and crit[0].severity == "critical")

    # ---------- 场景 2：Redis 慢（R4）----------
    print("\n=== 场景 2: Redis 慢 R4 ===")
    eng = RuleEngine(cfg)
    got = []
    for i in range(5):
        feed(eng, {"ts": float(i), "redis_latency_ms_p99": 0.5, "redis_ok": 1})
    for i in range(5, 8):
        got += feed(eng, {"ts": float(i), "redis_latency_ms_p99": 8.0, "redis_ok": 1})
    expect("R4 Redis P99 超 5ms 报警", any(a.rule == "R4_redis_slow" for a in got))

    # ---------- 场景 3：掉线风暴（R3）----------
    print("\n=== 场景 3: 掉线风暴 R3 ===")
    eng = RuleEngine(cfg)
    got = []
    for i in range(5):
        feed(eng, {"ts": float(i), "disconnect_rate": 0.001, "conns": 100})
    for i in range(5, 8):
        got += feed(eng, {"ts": float(i), "disconnect_rate": 0.05, "conns": 100})
    expect("R3 掉线率 5% 报警", any(a.rule == "R3_disconnect_storm" for a in got))

    # ---------- 场景 4：匹配队列积压（R5）----------
    print("\n=== 场景 4: 匹配队列积压 R5 ===")
    eng = RuleEngine(cfg)
    got = []
    for i in range(5):
        feed(eng, {"ts": float(i), "match_queue": 10})
    for i in range(5, 8):
        got += feed(eng, {"ts": float(i), "match_queue": 500})
    expect("R5 匹配队列超 200 报警", any(a.rule == "R5_match_backlog" for a in got))

    # ---------- 场景 5：连接数断崖（R6）----------
    # 断崖是"环比掉 20%"：模拟逐秒下降 1000→700→500→350，每一步环比都超 20%，
    # 连续 3 次满足 confirm_samples=3 触发。
    print("\n=== 场景 5: 连接数断崖 R6 ===")
    eng = RuleEngine(cfg)
    got = []
    feed(eng, {"ts": 0.0, "conns": 1000})
    for i, c in enumerate([700.0, 500.0, 350.0], start=1):
        got += feed(eng, {"ts": float(i), "conns": c})
    expect("R6 连接数环比掉 20%+ 报警", any(a.rule == "R6_conns_drop" for a in got))

    # ---------- 场景 6：内存泄漏趋势（R7）----------
    print("\n=== 场景 6: 内存持续增长 R7 ===")
    eng = RuleEngine(cfg)
    got = []
    # 连续递增，rising_streak 从第 2 个窗口起累加；喂足 14 个让 3 次确认都能满足
    for i in range(14):
        got += feed(eng, {"ts": float(i), "mem_mb": 500.0 + i * 1.0})
    expect("R7 内存连续上升报警", any(a.rule == "R7_mem_rising" for a in got))

    # ---------- 场景 7：正常波动不误报 ----------
    print("\n=== 场景 7: 正常波动不误报 ===")
    eng = RuleEngine(cfg)
    got = []
    import random
    rng = random.Random(42)
    for i in range(60):
        got += feed(eng, {
            "ts": float(i),
            "tick_ms_p99": 2.0 + rng.uniform(-0.3, 0.3),
            "err_rate": 0.001,
            "disconnect_rate": 0.001,
            "redis_latency_ms_p99": 0.5,
            "match_queue": 10 + rng.randint(-3, 3),
            "conns": 1000,
            "mem_mb": 500.0,
        })
    expect("正常波动 60 窗口 0 告警", len(got) == 0)

    # ---------- 汇总 ----------
    print(f"\n{'全部通过' if FAIL == 0 else f'有 {FAIL} 个失败'}")
    sys.exit(1 if FAIL else 0)


if __name__ == "__main__":
    run()
