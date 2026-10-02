"""OpsAgent 第二层：根因分析。带全套工程护栏。

诚实边界（面试必须自己先说出来，别等对方问）：
  · LLM 那一层是**调外部 API**，模型不是我的，我也没微调；
  · 真正属于我的工程是：异常检测规则、日志管道、上下文组装、
    JSON schema 约束与校验、超时/重试/降级/成本记账这一整套护栏。

本文件先实现**纯规则降级版**（RuleOnlyAnalyzer）：不依赖任何外部 API，
根据告警规则直接映射到根因枚举。它同时是：
  1. 离线可验证的核心（故障注入命中率就是用它算的）；
  2. LLM 路径三道护栏里的"降级"分支 —— LLM 挂了就退化成它，链路永远有输出。

真实 LLM 路径（LLMAnalyzer）留了接口：接外部 API 时实现 `call_llm()`，
把 SYSTEM_PROMPT + 上下文组装 + JSON 校验加进来即可（见 6.7.4/6.7.5）。
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any

# ============================================================ 报告结构

# 根因枚举：故障注入的 ground_truth 就从这里取值。模型/规则都不许自造。
ROOT_CAUSES = [
    "logic_tick_stall",        # 逻辑帧卡顿
    "client_disconnect_storm", # 掉线风暴
    "redis_unavailable",       # Redis 不可用
    "room_leak",               # 房间对象泄漏
    "match_queue_backlog",     # 匹配队列积压
    "unknown",
]

# 动作白名单：**只有这里列出的动作才允许被建议**（防模型幻觉出 rm -rf 级别动作）。
ACTIONS = [
    "fuse_room",            # 熔断异常房间
    "enable_rate_limit",    # 开启限流
    "degrade_redis_cache",  # Redis 降级（本地缓存兜底）
    "relax_match_mmr",      # 放宽匹配分数区间
    "scale_out",            # 扩容
    "noop",                 # 观察
]


@dataclass
class Report:
    root_cause: str = "unknown"
    confidence: float = 0.0
    evidence: list[str] = field(default_factory=list)
    suggested_actions: list[dict[str, Any]] = field(default_factory=list)

    def to_dict(self) -> dict[str, Any]:
        return {
            "root_cause": self.root_cause,
            "confidence": self.confidence,
            "evidence": self.evidence,
            "suggested_actions": self.suggested_actions,
        }


@dataclass
class Analysis:
    """一条告警的分析结果：根因报告 + 元信息（用了哪条规则、是否降级）。"""
    report: Report
    rule: str
    degraded: bool = False          # True = 走了纯规则降级（LLM 不可用）
    llm_prompt_tokens: int = 0
    llm_completion_tokens: int = 0

    def to_dict(self) -> dict[str, Any]:
        return {
            "report": self.report.to_dict(),
            "rule": self.rule,
            "degraded": self.degraded,
            "llm_prompt_tokens": self.llm_prompt_tokens,
            "llm_completion_tokens": self.llm_completion_tokens,
        }


# ============================================================ 纯规则降级分析器

# 规则 → 根因 + 动作。这是"降级"分支的逻辑，也是离线命中率的依据。
RULE_TO_ROOT_CAUSE = {
    "R1_tick_stall": ("logic_tick_stall", ["fuse_room"]),
    "R2_error_spike": ("unknown", ["noop"]),
    "R3_disconnect_storm": ("client_disconnect_storm", ["enable_rate_limit"]),
    "R4_redis_down": ("redis_unavailable", ["degrade_redis_cache"]),
    "R4_redis_slow": ("redis_unavailable", ["degrade_redis_cache"]),
    "R5_match_backlog": ("match_queue_backlog", ["relax_match_mmr"]),
    "R6_conns_drop": ("client_disconnect_storm", ["enable_rate_limit"]),
    "R7_mem_rising": ("room_leak", ["fuse_room"]),
}


class RuleOnlyAnalyzer:
    """纯规则降级分析器：告警规则 → 根因枚举 + 动作草案。

    不依赖任何外部 API。它是 LLM 路径失败时的降级分支，
    也是"故障注入命中率"离线验证用的分析器。
    """

    def analyze(self, alert: dict[str, Any], log_summary: dict[str, Any],
                metrics: dict[str, Any], reason: str = "rule_only") -> Analysis:
        rule = alert.get("rule", "unknown")
        cause, actions = RULE_TO_ROOT_CAUSE.get(rule, ("unknown", ["noop"]))
        evidence = []
        if log_summary and log_summary.get("top_events"):
            top = log_summary["top_events"][0]
            evidence.append(f"top 日志事件: {top[0]} × {top[1]}")
        if metrics:
            evidence.append(f"指标快照: ts={metrics.get('ts')}")
        if not evidence:
            evidence.append(f"命中规则 {rule}")

        report = Report(
            root_cause=cause,
            confidence=0.9 if cause != "unknown" else 0.3,
            evidence=evidence,
            suggested_actions=[{"action": a, "risk": "medium", "rollback": "observe 5min"} for a in actions],
        )
        return Analysis(report=report, rule=rule, degraded=True)


# ============================================================ LLM 路径（接口 + 桩）

# prompt 版本号：改 prompt 必须同时改这个号（出了事故要能回答"这条结论是哪一版 prompt 给的"）。
PROMPT_VERSION = "opsagent-prompt-v3"


class LLMAnalyzer:
    """真实 LLM 路径。call_llm() 是接外部 API 的唯一入口。

    三道护栏的顺序很关键：
      1. 超时（3 秒）—— 告警链路不能等一个可能永远不回的 HTTP 请求；
      2. 重试 1 次 —— 只重试"可以被修复的错误"（超时/5xx/JSON 不合法）；
      3. 降级 —— 三次都失败就退化成纯规则结论（RuleOnlyAnalyzer），链路永远有输出。
    降级不是"兜底失败"，它是这个系统的正常运行模式之一。
    """

    def __init__(self) -> None:
        self._fallback = RuleOnlyAnalyzer()
        self.timeout_s = 3.0

    def analyze(self, alert: dict[str, Any], log_summary: dict[str, Any],
                metrics: dict[str, Any], reason: str = "llm") -> Analysis:
        # 1. 尝试 LLM（真实实现里：组上下文 → 调 API → 超时/重试 → 解析 JSON → schema 校验）
        # 2. 失败则降级
        # 这里桩实现：直接降级（没有 API key 也能验证整条链路），
        # 真实接入时把 self._try_llm(...) 的注释解开。
        # return self._try_llm(alert, log_summary, metrics)
        return self._fallback.analyze(alert, log_summary, metrics, reason="llm_degraded")

    def _try_llm(self, alert: dict[str, Any], log_summary: dict[str, Any],
                 metrics: dict[str, Any]) -> Analysis:
        """真实实现：组装 USER_TEMPLATE 上下文 → urllib 调外部 API → 校验。
        返回的 Analysis 里 degraded=False。本桩不实现（需 API key）。
        """
        raise NotImplementedError("接外部 API 后实现；当前走降级路径")
