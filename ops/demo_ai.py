"""OpsAgent 完整链路演示：规则层报警 → AI 根因分析（真实 DeepSeek API）。

用法（先设好环境变量）：
    set DEEPSEEK_API_KEY=sk-xxx
    python ops/demo_ai.py --scenario F01
"""
from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from analyzers import RuleConfig, RuleEngine, read_jsonl, slice_logs, summarize_logs
from llm_analyzer import LLMAnalyzer


def main() -> int:
    ap = argparse.ArgumentParser(description="OpsAgent AI 链路演示")
    ap.add_argument("--scenario", default="F01", help="F01~F05")
    ap.add_argument("--scenarios", default=os.path.join("ops_state", "scenarios"))
    args = ap.parse_args()

    key = os.environ.get("DEEPSEEK_API_KEY", "")
    print(f"DEEPSEEK_API_KEY: {'已设置' if key else '未设置（将走降级路径）'}")

    sdir = os.path.join(args.scenarios, args.scenario)
    if not os.path.isdir(sdir):
        print(f"场景不存在：{sdir}，先跑 python inject_fault.py")
        return 1

    gt = json.load(open(os.path.join(sdir, "ground_truth.json"), encoding="utf-8"))
    metrics = list(read_jsonl(os.path.join(sdir, "metrics.jsonl")))
    logs = list(read_jsonl(os.path.join(sdir, "logs.jsonl")))

    print(f"\n=== 场景 {args.scenario}: {gt['name']}（期望根因: {gt['root_cause']}）===")
    win_lo, win_hi = gt["fault_window"]

    engine = RuleEngine(RuleConfig())
    analyzer = LLMAnalyzer()

    # 找故障窗内第一条 critical 告警
    picked = None
    for m in metrics:
        for alert in engine.on_metric(m):
            a = alert.to_dict()
            if not (win_lo <= a["ts"] <= win_hi):
                continue
            summary = summarize_logs(slice_logs(logs, a["ts"], 10, 5, 200))
            analysis = analyzer.analyze(a, summary, m)
            record = {"alert": a, "analysis": analysis, "log_summary": summary}
            if picked is None or a["severity"] == "critical":
                picked = record
            if a["severity"] == "critical":
                break
        if picked:
            break

    if picked is None:
        print("故障窗内无告警")
        return 1

    a = picked["alert"]
    an = picked["analysis"]
    print(f"\n--- 告警（ts={a['ts']}）---")
    print(f"  规则: {a['rule']}  严重级: {a['severity']}")
    print(f"  消息: {a['message']}")
    print(f"\n--- AI 分析（{'真实 DeepSeek' if not an.degraded else '降级(纯规则)'}）---")
    print(f"  根因: {an.report.root_cause}  置信度: {an.report.confidence}")
    print("  证据:")
    for e in an.report.evidence:
        print(f"    · {e}")
    print("  动作草案:")
    for act in an.report.suggested_actions:
        print(f"    · {act['action']} (风险 {act['risk']}, 回滚: {act['rollback']})")
    if not an.degraded:
        print(f"  LLM token: prompt={an.llm_prompt_tokens}, completion={an.llm_completion_tokens}")
    print(f"\n期望根因: {gt['root_cause']}  ->  {'命中 ✓' if an.report.root_cause == gt['root_cause'] else '未命中 ✗'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
