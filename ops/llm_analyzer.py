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


# ============================================================ LLM 路径（真实 API + 护栏）

# prompt 版本号：改 prompt 必须同时改这个号（出了事故要能回答"这条结论是哪一版 prompt 给的"）。
PROMPT_VERSION = "opsagent-prompt-v3"

SYSTEM_PROMPT = """你是 ArenaCore 游戏服务器的值班运维分析助手。你的唯一任务是根据给定的\
告警事件、服务器指标、日志摘要与原始日志行，判断最可能的根因，并给出可执行的处理动作草案。

硬性要求：
1. 只输出一个 JSON 对象，不要输出任何解释文字，不要用 markdown 代码块包裹。
2. root_cause 必须从给定的候选枚举里选一个，不允许自己造新词。
3. evidence 里的每一条都必须能在给定的日志或指标里找到对应，禁止脑补。
   如果证据不足以定位根因，就选 unknown 并把 confidence 写成 0.3 以下。
4. 不允许编造 uid、room_id、时间戳、日志行。所有数字只能来自输入。
5. suggested_actions 只能是候选动作白名单里的动作。你产出的是**草案**，
   必须由人确认后才会执行，所以宁可少给动作，也不要给危险动作。
"""

USER_TEMPLATE = """## 告警事件
{alert_json}

## 告警时刻的服务器指标
{metrics_json}

## 日志摘要（{slice_seconds} 秒窗口内 {total_lines} 行，按 event 聚合）
{log_summary_json}

## 候选根因枚举（只能从这里选）
{root_causes_json}

## 候选动作白名单（只能从这里选）
{actions_json}
"""


class LLMAnalyzer:
    """真实 LLM 路径（DeepSeek API，兼容 OpenAI 格式）。

    **诚实边界（面试必须自己先说）**：
    LLM 那一层是**调外部 API**（DeepSeek），模型不是自研的；
    自研的是异常检测规则、日志管道、上下文组装、JSON 约束与校验、
    超时/降级/成本记账这一整套护栏。

    三道护栏：
      1. 超时（15 秒）—— 告警链路不能等一个可能永远不回的 HTTP 请求；
      2. 解析失败 → 降级 —— 只降级不重试无限次（告警链路要快）；
      3. 降级 —— 失败就退化成纯规则结论（RuleOnlyAnalyzer），链路永远有输出。
      降级不是"兜底失败"，它是这个系统的正常运行模式之一。

    **密钥安全**：从环境变量 DEEPSEEK_API_KEY 读，绝不写进代码或提交。
    """

    def __init__(self) -> None:
        self._fallback = RuleOnlyAnalyzer()
        self.timeout_s = 15.0
        self._client = None

    def _get_client(self):
        import os
        from openai import OpenAI
        if self._client is None:
            key = os.environ.get("DEEPSEEK_API_KEY", "")
            if not key:
                return None
            self._client = OpenAI(api_key=key, base_url="https://api.deepseek.com")
        return self._client

    def analyze(self, alert: dict[str, Any], log_summary: dict[str, Any],
                metrics: dict[str, Any], reason: str = "llm") -> Analysis:
        client = self._get_client()
        if client is None:
            # 没有 key：降级（这是正常模式之一，不是失败）
            return self._fallback.analyze(alert, log_summary, metrics, reason="llm_no_key")
        try:
            return self._try_llm(client, alert, log_summary, metrics)
        except Exception:
            # 护栏 3：任何异常（超时/网络/解析）都降级，链路永远有输出
            return self._fallback.analyze(alert, log_summary, metrics, reason="llm_failed")

    def _try_llm(self, client, alert: dict[str, Any], log_summary: dict[str, Any],
                 metrics: dict[str, Any]) -> Analysis:
        import json as _json
        user = USER_TEMPLATE.format(
            alert_json=_json.dumps(alert, ensure_ascii=False),
            metrics_json=_json.dumps(metrics, ensure_ascii=False),
            slice_seconds=15,
            total_lines=log_summary.get("total_lines", 0),
            log_summary_json=_json.dumps(log_summary, ensure_ascii=False),
            root_causes_json=_json.dumps(ROOT_CAUSES, ensure_ascii=False),
            actions_json=_json.dumps(ACTIONS, ensure_ascii=False),
        )
        resp = client.chat.completions.create(
            model="deepseek-chat",
            messages=[
                {"role": "system", "content": SYSTEM_PROMPT},
                {"role": "user", "content": user},
            ],
            max_tokens=400,
            timeout=self.timeout_s,
        )
        text = resp.choices[0].message.content or ""
        # 去掉可能的 markdown 代码块包裹，再解析 JSON
        text = text.strip()
        if text.startswith("```"):
            text = text.strip("`")
            if text.startswith("json"):
                text = text[4:]
            text = text.strip()
        data = _json.loads(text)

        # schema 校验：root_cause 必须在枚举里，动作必须在白名单里
        cause = data.get("root_cause", "unknown")
        if cause not in ROOT_CAUSES:
            cause = "unknown"
        conf = float(data.get("confidence", 0.0))
        evidence = [str(e) for e in data.get("evidence", [])[:5]]
        actions = []
        for a in data.get("suggested_actions", [])[:3]:
            if isinstance(a, dict):
                act = a.get("action", "noop")
                if act not in ACTIONS:
                    act = "noop"
                actions.append({"action": act,
                                "risk": str(a.get("risk", "low")),
                                "rollback": str(a.get("rollback", "observe 5min"))})
            elif isinstance(a, str) and a in ACTIONS:
                actions.append({"action": a, "risk": "low", "rollback": "observe 5min"})

        report = Report(root_cause=cause, confidence=conf,
                        evidence=evidence, suggested_actions=actions)
        usage = getattr(resp, "usage", None)
        pt = int(getattr(usage, "prompt_tokens", 0) or 0)
        ct = int(getattr(usage, "completion_tokens", 0) or 0)
        return Analysis(report=report, rule=str(alert.get("rule", "")),
                        degraded=False, llm_prompt_tokens=pt, llm_completion_tokens=ct)
