"""评估命中率：跑完 5 个注入场景，算"根因命中 N/5"。

评估标准必须先写清楚，否则"命中 3/5"这句话没法被验证。这里定的是：
    在故障时间窗内，**第一条 critical 告警**产出的报告，其 root_cause
    与 ground_truth.json 里的期望值一致 → 记命中。
没有 critical 告警时，退而取故障窗内的第一条告警。
（为什么取"故障窗内的第一条"：真出事的时候人也是看第一条告警，
 后面几百条都是它的回声。用"任意一条命中就算命中"会把命中率刷成 5/5，
 那是自欺欺人。）

用法：
    python inject_fault.py --out ops_state/scenarios
    python eval_faults.py --scenarios ops_state/scenarios
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from typing import Any

from analyzers import RuleConfig, RuleEngine, read_jsonl, slice_logs, summarize_logs
from llm_analyzer import RuleOnlyAnalyzer


def evaluate_one(scenario_dir: str) -> dict[str, Any]:
    gt = json.load(open(os.path.join(scenario_dir, "ground_truth.json"), encoding="utf-8"))
    metrics = list(read_jsonl(os.path.join(scenario_dir, "metrics.jsonl")))
    logs = list(read_jsonl(os.path.join(scenario_dir, "logs.jsonl")))

    engine = RuleEngine(RuleConfig())
    analyzer = RuleOnlyAnalyzer()
    win_lo, win_hi = gt["fault_window"]
    first_critical: dict[str, Any] | None = None
    first_any: dict[str, Any] | None = None
    all_alerts: list[dict[str, Any]] = []

    for m in metrics:
        for alert in engine.on_metric(m):
            a = alert.to_dict()
            all_alerts.append(a)
            if not (win_lo <= a["ts"] <= win_hi):
                continue
            summary = summarize_logs(slice_logs(logs, a["ts"], 10, 5, 200))
            analysis = analyzer.analyze(a, summary, m, reason="eval")
            record = {"alert": a, "analysis": analysis.to_dict(), "log_summary": summary}
            if first_any is None:
                first_any = record
            if a["severity"] == "critical" and first_critical is None:
                first_critical = record

    picked = first_critical or first_any
    hit = False
    if picked is not None:
        hit = picked["analysis"]["report"]["root_cause"] == gt["root_cause"]
    return {
        "scenario": gt["scenario"],
        "name": gt["name"],
        "expected": gt["root_cause"],
        "got": None if picked is None else picked["analysis"]["report"]["root_cause"],
        "hit": hit,
        "picked_rule": None if picked is None else picked["alert"]["rule"],
        "alert_count": len(all_alerts),
        "picked": picked,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description="OpsAgent 根因命中率评估")
    ap.add_argument("--scenarios", default=os.path.join("ops_state", "scenarios"))
    ap.add_argument("--show", action="store_true", help="打印每个场景选中的报告全文")
    args = ap.parse_args()

    dirs = sorted(d for d in os.listdir(args.scenarios)
                  if os.path.isdir(os.path.join(args.scenarios, d))
                  and os.path.exists(os.path.join(args.scenarios, d, "ground_truth.json")))
    if not dirs:
        print(f"{args.scenarios} 下没有场景，先跑 python inject_fault.py")
        return 1

    hits = 0
    print(f"{'场景':<6}{'名称':<16}{'期望根因':<26}{'实际判定':<26}{'结果':<6}{'告警数'}")
    print("-" * 96)
    for d in dirs:
        r = evaluate_one(os.path.join(args.scenarios, d))
        hits += 1 if r["hit"] else 0
        print(f"{r['scenario']:<6}{r['name']:<16}{r['expected']:<26}"
              f"{str(r['got']):<26}{'命中' if r['hit'] else '未命中':<6}{r['alert_count']}")
        if args.show and r["picked"]:
            print("   证据:", " | ".join(r["picked"]["analysis"]["report"]["evidence"][:2]))
            print("   动作:", [a["action"] for a in r["picked"]["analysis"]["report"]["suggested_actions"]])
    total = len(dirs)
    print("-" * 96)
    print(f"根因命中率: {hits}/{total} = {hits / total * 100:.0f}%")
    return 0


if __name__ == "__main__":
    sys.exit(main())
