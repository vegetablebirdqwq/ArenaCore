"""OpsAgent 第一层：纯规则异常检测。不依赖任何第三方库。

这一层是整个运营支撑链路的**地基**，它必须满足三个条件：
 1. 确定：同样的输入永远给同样的输出。告警不能靠概率。
 2. 便宜：每 5 秒跑一次，CPU 占用可以忽略。
 3. 能在压测里被验证：注入一个已知故障，它必须报出对应的规则。

LLM 只负责"看到一堆告警之后猜根因"，告警本身绝不允许 LLM 参与 ——
否则你连"系统到底有没有出事"这件事都变得不可复现了。
"""

from __future__ import annotations

import json
import math
from collections import Counter, deque
from dataclasses import dataclass, field
from typing import Any, Iterable, Sequence

# ============================================================ 基础统计


def percentile(values: Sequence[float], p: float) -> float:
    """线性插值分位点。p ∈ [0, 100]。

    为什么不用 statistics.quantiles：它按"等分桶取边界"算，
    样本少的时候（比如一个 30 个点的滑窗）给出的数会跳变，
    对监控曲线很不友好。自己写 8 行，行为完全可解释。
    """
    if not values:
        return 0.0
    xs = sorted(values)
    if len(xs) == 1:
        return xs[0]
    k = (len(xs) - 1) * (p / 100.0)
    lo = int(math.floor(k))
    hi = int(math.ceil(k))
    if lo == hi:
        return float(xs[lo])
    return xs[lo] * (hi - k) + xs[hi] * (k - lo)


class SlidingWindow:
    """按"条数"滚动的窗口（指标是定频上报的，所以条数≈时间）。

    它解决什么问题：单点采样会抖。
    某一秒 P99 突然 20ms 可能只是一次磁盘刷盘，连续 5 个窗口都高才是真出事。
    窗口本身就是最便宜的"去抖滤波器"，不用上任何算法。
    """

    def __init__(self, maxlen: int = 60) -> None:
        self._dq: deque[float] = deque(maxlen=maxlen)

    def push(self, value: float) -> None:
        self._dq.append(value)

    def values(self) -> list[float]:
        return list(self._dq)

    def latest(self) -> float:
        return self._dq[-1] if self._dq else 0.0

    def rising_streak(self) -> int:
        """末尾连续递增的个数。用来抓"内存/队列在持续涨"这类趋势。"""
        n = 0
        vals = list(self._dq)
        for i in range(len(vals) - 1, 0, -1):
            if vals[i] > vals[i - 1]:
                n += 1
            else:
                break
        return n

    def __len__(self) -> int:
        return len(self._dq)


class EwmaDetector:
    """EWMA（指数加权移动平均）+ 3σ 突变检测。

    它解决什么问题：固定阈值只能抓"超过 200ms"这种已知的坑。
    如果系统本来就在 5ms 附近抖，某天悄悄变成 15ms —— 没超任何阈值，
    但行为已经变了（可能是内存泄漏的早期、可能是某台机器换了调度）。
    拿"最近的样本"跟"自己的历史波动范围"比，才抓得到这种事。

    EWMA 而不是普通均值：均值要存一整个窗口，而且要等窗口填满才有意义；
    EWMA 只存两个数（均值、方差），对最近的变化更敏感，天生适合流式。
    alpha 越大越敏感、越容易误报，0.2~0.3 是常规取值。
    """

    def __init__(self, alpha: float = 0.3, warmup: int = 20, z_threshold: float = 3.0) -> None:
        self.alpha = alpha
        self.warmup = warmup
        self.z_threshold = z_threshold
        self.mean: float | None = None
        self.var: float = 0.0
        self.n: int = 0

    def update(self, x: float) -> tuple[bool, float, float]:
        """返回 (是否突变, 当前均值, 当前标准差)。"""
        if self.mean is None:
            self.mean = x
            self.var = 0.0
            self.n = 1
            return False, x, 0.0
        self.n += 1
        delta = x - self.mean
        self.mean += self.alpha * delta
        # 方差也用同样的指数方式更新：var <- (1-a)*var + a*delta^2
        self.var += self.alpha * (delta * delta - self.var)
        sigma = math.sqrt(max(self.var, 0.0))
        if self.n < self.warmup or sigma <= 1e-9:
            return False, self.mean, sigma
        z = abs(x - self.mean) / sigma
        return z >= self.z_threshold, self.mean, sigma


# ============================================================ 告警


@dataclass
class Alert:
    ts: float
    rule: str
    severity: str           # "warn" | "critical"
    metric: str
    value: float
    threshold: float
    message: str
    context: dict[str, Any] = field(default_factory=dict)

    def to_dict(self) -> dict[str, Any]:
        return {
            "ts": self.ts,
            "rule": self.rule,
            "severity": self.severity,
            "metric": self.metric,
            "value": self.value,
            "threshold": self.threshold,
            "message": self.message,
            "context": self.context,
        }


@dataclass
class RuleConfig:
    """阈值集中放一处。写死进代码的阈值是没法在面试里解释的，
    所以每条阈值后面都写清楚"为什么是这个数"。"""

    # 30Hz = 每帧 33.3ms 预算。P99 超过 1/4 预算（8ms）就该警告，
    # 超过一半（15ms）说明已经开始挤压别的阶段的时间了。
    tick_p99_warn: float = 8.0        # ms
    tick_p99_crit: float = 15.0       # ms
    # 逻辑层错误率（结算异常/非法包/内部断言）
    err_rate_warn: float = 0.01
    err_rate_crit: float = 0.05
    # 掉线率 = 每秒掉线数 / 在线数
    disconnect_warn: float = 0.02
    disconnect_crit: float = 0.10
    # Redis 单命令 P99，内网正常在 1ms 以内
    redis_p99_warn: float = 5.0       # ms
    redis_p99_crit: float = 20.0      # ms
    # 匹配队列积压
    match_queue_warn: float = 200.0
    match_queue_crit: float = 1000.0
    # 连接数断崖：环比掉 20%
    conns_drop_ratio: float = 0.20
    # 内存持续增长：连续 10 个窗口递增就报（5s 一条 = 50 秒）
    mem_rising_streak: int = 10

    # 去抖与抑制
    confirm_samples: int = 3          # 连续 3 条指标都超阈才报
    cooldown_s: float = 60.0          # 同一规则 60 秒内只报一次


class RuleEngine:
    """把指标流变成告警事件。接口只有一个：喂一条指标，可能吐 0..N 条告警。"""

    WATCHED = ("tick_ms_p99", "err_rate", "disconnect_rate", "match_queue",
               "conns", "redis_latency_ms_p99", "mem_mb", "recv_qps")

    def __init__(self, config: RuleConfig | None = None, window: int = 60) -> None:
        self.cfg = config or RuleConfig()
        self.windows: dict[str, SlidingWindow] = {k: SlidingWindow(window) for k in self.WATCHED}
        self.streaks: dict[str, int] = {}
        self.last_fired: dict[str, float] = {}
        self.mutators = {
            "tick_ms_p99": EwmaDetector(),
            "disconnect_rate": EwmaDetector(),
            "redis_latency_ms_p99": EwmaDetector(),
        }
        self.prev_conns: float | None = None

    # -------------------------------------------------- 内部工具
    def _confirm(self, rule: str, fired: bool, ts: float) -> bool:
        """去抖 + 冷却。fired 是"这一刻超阈了吗"，这里决定"该不该报警"。"""
        if not fired:
            self.streaks[rule] = 0
            return False
        self.streaks[rule] = self.streaks.get(rule, 0) + 1
        if self.streaks[rule] < self.cfg.confirm_samples:
            return False
        if ts - self.last_fired.get(rule, -1e18) < self.cfg.cooldown_s:
            return False
        self.last_fired[rule] = ts
        return True

    def _window_ctx(self, ts: float) -> dict[str, Any]:
        ticks = self.windows["tick_ms_p99"].values()
        conns_vals = self.windows["conns"].values()
        return {
            "window_samples": len(ticks),
            "tick_p99_window_max": max(ticks) if ticks else 0.0,
            "tick_p99_window_p50": percentile(ticks, 50.0),
            "conns_now": self.windows["conns"].latest(),
            "conns_window_min": min(conns_vals) if conns_vals else 0.0,
        }

    # -------------------------------------------------- 主入口
    def on_metric(self, m: dict[str, Any]) -> list[Alert]:
        ts = float(m.get("ts", 0.0))
        out: list[Alert] = []
        for key in self.WATCHED:
            if key in m:
                self.windows[key].push(float(m[key]))

        tick = float(m.get("tick_ms_p99", 0.0))
        err = float(m.get("err_rate", 0.0))
        disc = float(m.get("disconnect_rate", 0.0))
        redis = float(m.get("redis_latency_ms_p99", 0.0))
        queue = float(m.get("match_queue", 0.0))
        conns = float(m.get("conns", 0.0))
        redis_ok = bool(m.get("redis_ok", 1))
        ctx = self._window_ctx(ts)

        def add(rule: str, severity: str, metric: str, value: float, threshold: float,
                message: str, extra: dict[str, Any] | None = None) -> None:
            c = dict(ctx)
            if extra:
                c.update(extra)
            out.append(Alert(ts, rule, severity, metric, value, threshold, message, c))

        # R1 逻辑帧卡顿
        if self._confirm("R1_tick_stall",
                         tick >= self.cfg.tick_p99_warn, ts):
            sev = "critical" if tick >= self.cfg.tick_p99_crit else "warn"
            add("R1_tick_stall", sev, "tick_ms_p99", tick, self.cfg.tick_p99_warn,
                f"逻辑帧 P99 {tick:.1f}ms 超过 {self.cfg.tick_p99_warn}ms 预算阈值")

        # R2 错误率
        if self._confirm("R2_error_spike", err >= self.cfg.err_rate_warn, ts):
            sev = "critical" if err >= self.cfg.err_rate_crit else "warn"
            add("R2_error_spike", sev, "err_rate", err, self.cfg.err_rate_warn,
                f"错误率 {err * 100:.2f}% 超过 {self.cfg.err_rate_warn * 100:.1f}%")

        # R3 掉线率
        if self._confirm("R3_disconnect_storm", disc >= self.cfg.disconnect_warn, ts):
            sev = "critical" if disc >= self.cfg.disconnect_crit else "warn"
            add("R3_disconnect_storm", sev, "disconnect_rate", disc, self.cfg.disconnect_warn,
                f"掉线率 {disc * 100:.2f}% 超过 {self.cfg.disconnect_warn * 100:.1f}%")

        # R4 Redis 异常（直接报，不做去抖：Redis 挂了没什么好犹豫的）
        if not redis_ok and self._confirm("R4_redis_down", True, ts):
            add("R4_redis_down", "critical", "redis_ok", 0.0, 1.0, "Redis 健康检查失败")
        elif self._confirm("R4_redis_slow", redis >= self.cfg.redis_p99_warn, ts):
            sev = "critical" if redis >= self.cfg.redis_p99_crit else "warn"
            add("R4_redis_slow", sev, "redis_latency_ms_p99", redis, self.cfg.redis_p99_warn,
                f"Redis 命令 P99 {redis:.1f}ms 超过 {self.cfg.redis_p99_warn}ms")

        # R5 匹配队列积压
        if self._confirm("R5_match_backlog", queue >= self.cfg.match_queue_warn, ts):
            sev = "critical" if queue >= self.cfg.match_queue_crit else "warn"
            add("R5_match_backlog", sev, "match_queue", queue, self.cfg.match_queue_warn,
                f"匹配队列积压 {queue:.0f} 超过 {self.cfg.match_queue_warn:.0f}")

        # R6 连接数断崖
        if self.prev_conns is not None and self.prev_conns > 10:
            drop = (self.prev_conns - conns) / self.prev_conns
            if self._confirm("R6_conns_drop", drop >= self.cfg.conns_drop_ratio, ts):
                add("R6_conns_drop", "critical", "conns", conns, self.cfg.conns_drop_ratio,
                    f"连接数环比下降 {drop * 100:.1f}%（{self.prev_conns:.0f} → {conns:.0f}）")
        self.prev_conns = conns

        # R7 内存/房间持续增长（趋势类，不靠阈值）
        rising = self.windows["mem_mb"].rising_streak()
        if self._confirm("R7_mem_rising", rising >= self.cfg.mem_rising_streak, ts):
            add("R7_mem_rising", "warn", "mem_mb", float(m.get("mem_mb", 0.0)),
                0.0, f"内存连续 {rising} 个采样上升，疑似泄漏", {"rising_streak": rising})

        # R8 3σ 突变（跟自己的历史比，不看固定阈值）
        for metric, det in self.mutators.items():
            if metric not in m:
                continue
            hit, mean, sigma = det.update(float(m[metric]))
            rule = f"R8_mutation_{metric}"
            if hit and self._confirm(rule, True, ts):
                add(rule, "warn", metric, float(m[metric]), mean,
                    f"{metric} 相对历史均值 {mean:.2f} 偏离超过 3σ（σ={sigma:.3f}）",
                    {"ewma_mean": mean, "ewma_sigma": sigma})
            else:
                self.streaks[rule] = 0
        return out


# ============================================================ 日志管道
# 日志是 JSONL（每行一个 JSON）。这三件事是 LLM 层的前置：解析、切片、聚合摘要。
# 关键设计原则：**摘要里的计数是自己算的，不是模型算的。** 模型只负责
# "从这些事实里推断根因"，不负责"数数"——否则它有机会编数字。


def read_jsonl(path: str) -> Iterable[dict[str, Any]]:
    """逐行读 JSONL，跳过空行和坏行。坏行直接跳过而不是让整个链路崩掉。"""
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                yield json.loads(line)
            except json.JSONDecodeError:
                continue


def slice_logs(logs: Sequence[dict[str, Any]], ts: float,
               before_s: float, after_s: float, max_lines: int) -> list[dict[str, Any]]:
    """取 [ts-before, ts+after] 秒内的日志，最多 max_lines 行（已按 ts 升序）。

    告警要能跟日志按时间对齐，靠的是 ts（epoch 秒）。切片窗口是"告警瞬间附近"
    的证据，给模型看太多会淹没注意力。
    """
    out = []
    for e in logs:
        t = float(e.get("ts", 0.0))
        if ts - before_s <= t <= ts + after_s:
            out.append(e)
            if len(out) >= max_lines:
                break
    return out


def summarize_logs(logs: Sequence[dict[str, Any]]) -> dict[str, Any]:
    """把日志切片压成摘要：总数、按级别、top 事件、活跃 uid、样本行。

    摘要的计数由 Python 算，模型只负责读。这让模型"没有机会编数字"。
    """
    total = len(logs)
    by_level: Counter[str] = Counter()
    by_event: Counter[str] = Counter()
    hot_uids: Counter[str] = Counter()
    samples: list[dict[str, Any]] = []
    for e in logs:
        by_level[e.get("level", "info")] += 1
        by_event[e.get("event", "unknown")] += 1
        uid = e.get("uid", 0)
        if uid:
            hot_uids[str(uid)] += 1
        if len(samples) < 16:
            samples.append(e)
    return {
        "total_lines": total,
        "by_level": dict(by_level),
        "top_events": by_event.most_common(10),
        "hot_uids": hot_uids.most_common(5),
        "samples": samples,
    }


def estimate_tokens(text: str) -> int:
    """粗略估算 token 数。中文 1 字 ≈ 1.5 token，ASCII ≈ 0.25 token。
    用途：控制送进 LLM 的上下文长度，别把 200 行日志全塞进去。
    """
    cjk = sum(1 for ch in text if '\u4e00' <= ch <= '\u9fff')
    ascii_chars = len(text) - cjk
    return int(cjk * 1.5 + ascii_chars * 0.25) + 1
