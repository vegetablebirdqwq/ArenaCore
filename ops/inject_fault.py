"""故障注入：造 5 种故障，顺便把"标准答案"写下来。

为什么必须这么做（面试里这是最能加分的一段）：
"OpsAgent 能定位根因"这句话本身是不可验证的。
只有先注入**已知**故障、把期望根因写成 ground_truth.json，
再让链路去跑，才能得出"命中 3/5"这种能写进简历、被追问也站得住的数字。
没有 ground truth 的 AI 项目，命中率只能靠编。

用法：
    python inject_fault.py --out ops_state/scenarios        # 生成 5 个场景
    python inject_fault.py --only F03 --out ops_state/scenarios
"""

from __future__ import annotations

import argparse
import json
import os
import random
from typing import Any, Callable

# 场景时间轴：0 ~ TOTAL_S 秒的基线 + FAULT_AT ~ FAULT_AT+DURATION_S 的故障期
TOTAL_S = 120
STEP_S = 5          # 指标每 5 秒一条
FAULT_AT = 40
FAULT_S = 60        # 故障持续 60 秒，保证"连续 3 个采样超阈"的去抖条件能过


def base_metrics(t: float, rng: random.Random) -> dict[str, Any]:
    """正常状态的指标。加一点抖动，让 3σ 检测器有波形可学。"""
    jitter = lambda a: rng.uniform(-a, a)  # noqa: E731
    return {
        "tick_ms_p99": 3.4 + jitter(0.6),
        "tick_ms_avg": 1.2 + jitter(0.2),
        "conns": 1000,
        "err_rate": 0.001 + abs(jitter(0.0004)),
        "disconnect_rate": 0.002 + abs(jitter(0.0008)),
        "recv_qps": 24000 + jitter(1500),
        "cpu": 0.45 + jitter(0.05),
        "mem_mb": 512 + t * 0.05,          # 正常也有非常缓慢的增长
        "rooms": 180,
        "redis_ok": 1,
        "redis_latency_ms_p99": 0.8 + abs(jitter(0.2)),
        "match_queue": 15 + jitter(5),
    }


def base_log(t: float, rng: random.Random) -> list[dict[str, Any]]:
    """正常状态的日志：每次采样点造 10~20 行常规事件。"""
    out = []
    n = rng.randint(10, 20)
    for i in range(n):
        uid = rng.randint(1000, 1200)
        ev = rng.choice(["session.login", "session.move", "room.tick", "match.enqueue",
                         "match.success", "room.settle"])
        out.append({
            "ts": t + i * 0.1,
            "level": "info",
            "event": ev,
            "uid": uid,
            "room": rng.randint(100, 200),
            "latency_us": rng.randint(200, 3000),
            "node": "logic-0",
            "msg": f"{ev} ok",
        })
    return out


# ============================================================ 5 种故障

def fault_f01(t: float, in_fault: bool, rng: random.Random, m: dict[str, Any],
              logs: list[dict[str, Any]]) -> None:
    """逻辑帧卡顿：tick P99 从 3.4ms 涨到 20ms 以上。"""
    if in_fault:
        m["tick_ms_p99"] = 20 + rng.uniform(-3, 6)
        m["tick_ms_avg"] = 9 + rng.uniform(-1, 2)
        m["err_rate"] = 0.012 + abs(rng.uniform(0, 0.01))
        for i in range(60):  # 故障期日志爆量
            logs.append({
                "ts": t + i * 0.05,
                "level": "warn",
                "event": "logic.frame_overrun",
                "uid": 0,
                "room": rng.randint(100, 200),
                "latency_us": int((20 + rng.uniform(0, 8)) * 1000),
                "node": "logic-0",
                "msg": f"tick budget exceeded: {20 + rng.uniform(0, 8):.1f}ms > 33.3ms budget*0.6",
            })


def fault_f02(t: float, in_fault: bool, rng: random.Random, m: dict[str, Any],
              logs: list[dict[str, Any]]) -> None:
    """掉线风暴：客户端大面积心跳超时。"""
    if in_fault:
        m["disconnect_rate"] = 0.12 + rng.uniform(0, 0.05)
        m["conns"] = 1000 - int((t - FAULT_AT) * 6)
        for i in range(120):
            logs.append({
                "ts": t + i * 0.02,
                "level": "warn",
                "event": "session.closed",
                "uid": rng.randint(1000, 1200),
                "room": rng.randint(100, 200),
                "latency_us": rng.randint(100, 500),
                "node": "gateway-0",
                "msg": "close reason=heartbeat_timeout, last_recv=5012ms",
            })


def fault_f03(t: float, in_fault: bool, rng: random.Random, m: dict[str, Any],
              logs: list[dict[str, Any]]) -> None:
    """Redis 不可用：连接被拒 / 命令超时。"""
    if in_fault:
        m["redis_ok"] = 0
        m["redis_latency_ms_p99"] = 35 + rng.uniform(0, 20)
        m["err_rate"] = 0.03 + rng.uniform(0, 0.01)
        for i in range(80):
            logs.append({
                "ts": t + i * 0.03,
                "level": "error",
                "event": "redis.command_timeout",
                "uid": 0,
                "room": 0,
                "latency_us": int((35 + rng.uniform(0, 20)) * 1000),
                "node": "logic-0",
                "msg": "ZADD arena:rank:weekly timed out after 3000ms, fallback to local buffer",
            })


def fault_f04(t: float, in_fault: bool, rng: random.Random, m: dict[str, Any],
              logs: list[dict[str, Any]]) -> None:
    """房间对象泄漏：房间创建了但从不释放，内存单调上涨。

    注意：这一条从第 0 秒就在涨 —— 泄漏类故障没有"突变的瞬间"，
    它只有趋势。所以对应的规则是 R7（连续 N 个采样递增），而不是阈值规则。
    """
    m["mem_mb"] = 512 + t * 1.8 + rng.uniform(-2, 2)
    m["rooms"] = 180 + int(t * 2.5)
    if int(t) % 15 == 0:
        for i in range(6):
            logs.append({
                "ts": t + i * 0.3,
                "level": "warn",
                "event": "room.not_released",
                "uid": 0,
                "room": 1000 + int(t) + i,
                "latency_us": 0,
                "node": "logic-0",
                "msg": f"room {1000 + int(t) + i} idle for 600s but still referenced, "
                       f"rooms_total={180 + int(t * 2.5)}",
            })


def fault_f05(t: float, in_fault: bool, rng: random.Random, m: dict[str, Any],
              logs: list[dict[str, Any]]) -> None:
    """匹配队列积压：匹配进程处理不过来 / 玩家段位过于分散。"""
    if in_fault:
        m["match_queue"] = 300 + (t - FAULT_AT) * 25
        m["cpu"] = 0.88 + rng.uniform(0, 0.08)
        for i in range(40):
            logs.append({
                "ts": t + i * 0.05,
                "level": "warn",
                "event": "match.timeout",
                "uid": rng.randint(1000, 1200),
                "room": 0,
                "latency_us": 0,
                "node": "match-0",
                "msg": f"match timed out after 30s, queue_depth={300 + int((t - FAULT_AT) * 25)}",
            })


SCENARIOS: dict[str, dict[str, Any]] = {
    "F01": {
        "name": "逻辑帧卡顿",
        "inject": fault_f01,
        "root_cause": "logic_tick_stall",
        "expect_rule": "R1_tick_stall",
        "expect_action": "fuse_room",
        "note": "tick P99 从 3.4ms 涨到 20ms+，日志出现 logic.frame_overrun 爆量",
    },
    "F02": {
        "name": "掉线风暴",
        "inject": fault_f02,
        "root_cause": "client_disconnect_storm",
        "expect_rule": "R3_disconnect_storm",
        "expect_action": "enable_rate_limit",
        "note": "掉线率 12%，日志 session.closed reason=heartbeat_timeout 爆量",
    },
    "F03": {
        "name": "Redis 不可用",
        "inject": fault_f03,
        "root_cause": "redis_unavailable",
        "expect_rule": "R4_redis_down",
        "expect_action": "degrade_redis_cache",
        "note": "健康检查失败 + 命令超时 35ms+",
    },
    "F04": {
        "name": "房间对象泄漏",
        "inject": fault_f04,
        "root_cause": "room_leak",
        "expect_rule": "R7_mem_rising",
        "expect_action": "fuse_room",
        "note": "内存全程单调上涨，日志 room.not_released",
    },
    "F05": {
        "name": "匹配队列积压",
        "inject": fault_f05,
        "root_cause": "match_queue_backlog",
        "expect_rule": "R5_match_backlog",
        "expect_action": "relax_match_mmr",
        "note": "队列从 300 涨到 1800",
    },
}


def generate(scenario_id: str, out_root: str) -> str:
    spec = SCENARIOS[scenario_id]
    # 种子必须是确定的：不能用 hash(scenario_id)，Python 的字符串 hash 每个进程都不同，
    # 那样两次生成的场景文件不一样，"命中率"就复现不出来了。
    seed = sum((i + 1) * ord(c) for i, c in enumerate(scenario_id))
    rng = random.Random(seed)
    out_dir = os.path.join(out_root, scenario_id)
    os.makedirs(out_dir, exist_ok=True)
    metrics_path = os.path.join(out_dir, "metrics.jsonl")
    logs_path = os.path.join(out_dir, "logs.jsonl")

    with open(metrics_path, "w", encoding="utf-8") as fm, open(logs_path, "w", encoding="utf-8") as fl:
        for step in range(0, TOTAL_S // STEP_S + 1):
            t = float(step * STEP_S)
            in_fault = FAULT_AT <= t <= FAULT_AT + FAULT_S
            m = base_metrics(t, rng)
            logs = base_log(t, rng)
            spec["inject"](t, in_fault, rng, m, logs)
            m["ts"] = t
            fm.write(json.dumps(m, ensure_ascii=False) + "\n")
            for e in logs:
                fl.write(json.dumps(e, ensure_ascii=False) + "\n")

    ground_truth = {
        "scenario": scenario_id,
        "name": spec["name"],
        "root_cause": spec["root_cause"],
        "expect_rule": spec["expect_rule"],
        "expect_action": spec["expect_action"],
        "fault_window": [float(FAULT_AT), float(FAULT_AT + FAULT_S)],
        "note": spec["note"],
    }
    with open(os.path.join(out_dir, "ground_truth.json"), "w", encoding="utf-8") as f:
        json.dump(ground_truth, f, ensure_ascii=False, indent=2)
    return out_dir


def main() -> int:
    ap = argparse.ArgumentParser(description="ArenaCore OpsAgent 故障注入")
    ap.add_argument("--out", default=os.path.join("ops_state", "scenarios"))
    ap.add_argument("--only", default=None, help="只生成某一个场景，例如 F03")
    args = ap.parse_args()

    ids = [args.only] if args.only else list(SCENARIOS)
    for sid in ids:
        path = generate(sid, args.out)
        print(f"生成 {sid} {SCENARIOS[sid]['name']} -> {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
