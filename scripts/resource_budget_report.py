#!/usr/bin/env python3
"""Generate a C-Claw ESP32S31 resource budget report from existing artifacts."""

from __future__ import annotations

import argparse
import json
import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


DEFAULT_THRESHOLDS = {
    "internal_largest_min": 64 * 1024,
    "psram_largest_min": 2 * 1024 * 1024,
    "stack_high_water_words_min": 512,
    "event_drop_max": 0,
}


def safe_int(value: Any, default: int = 0) -> int:
    """处理 scripts/resource_budget_report.py 中的 safe int 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def read_json(path: Path | None) -> dict[str, Any]:
    """处理 scripts/resource_budget_report.py 中的 read json 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    if not path or not path.exists():
        return {}
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return {}
    return value if isinstance(value, dict) else {}


def parse_profile_text(text: str) -> dict[str, Any] | None:
    """处理 scripts/resource_budget_report.py 中的 parse profile text 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    try:
        value = json.loads(text)
    except Exception:
        return None
    return value if isinstance(value, dict) else None


@dataclass
class Budget:
    map_flash_bytes: int | None = None
    map_dram_bytes: int | None = None
    map_iram_bytes: int | None = None
    sdkconfig_flags: dict[str, str] = field(default_factory=dict)
    min_internal_free: int | None = None
    min_internal_largest: int | None = None
    min_psram_free: int | None = None
    min_psram_largest: int | None = None
    min_stack_words: int | None = None
    max_event_dropped: int = 0
    max_event_maintenance_skipped: int = 0
    max_tool_waiting: int = 0
    ota_last_failure_code: str | None = None
    ota_min_psram_largest: int | None = None
    ota_min_internal_largest: int | None = None
    profile_samples: int = 0
    issues: list[str] = field(default_factory=list)

    def observe_min(self, attr: str, value: int) -> None:
        """处理 scripts/resource_budget_report.py 中的 observe min 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        if value < 0:
            return
        current = getattr(self, attr)
        setattr(self, attr, value if current is None else min(current, value))


def parse_map_file(path: Path | None, budget: Budget) -> None:
    """处理 scripts/resource_budget_report.py 中的 parse map file 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    if not path or not path.exists():
        return
    text = path.read_text(encoding="utf-8", errors="replace")
    patterns = {
        "map_flash_bytes": [r"\bflash\b[^\n]*?(\d+)\s+bytes", r"\.flash\w*\s+0x[0-9a-fA-F]+\s+(0x[0-9a-fA-F]+)"],
        "map_dram_bytes": [r"\bdram\b[^\n]*?(\d+)\s+bytes", r"\.dram\w*\s+0x[0-9a-fA-F]+\s+(0x[0-9a-fA-F]+)"],
        "map_iram_bytes": [r"\biram\b[^\n]*?(\d+)\s+bytes", r"\.iram\w*\s+0x[0-9a-fA-F]+\s+(0x[0-9a-fA-F]+)"],
    }
    for attr, regexes in patterns.items():
        total = 0
        for regex in regexes:
            for match in re.finditer(regex, text, re.IGNORECASE):
                raw = match.group(1)
                value = int(raw, 16) if raw.lower().startswith("0x") or re.search(r"[a-fA-F]", raw) else int(raw)
                total += value
            if total:
                break
        if total:
            setattr(budget, attr, total)


def parse_sdkconfig(path: Path | None, budget: Budget) -> None:
    """处理 scripts/resource_budget_report.py 中的 parse sdkconfig 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    if not path or not path.exists():
        return
    interesting = {
        "CONFIG_IDF_TARGET",
        "CONFIG_ESPTOOLPY_FLASHSIZE",
        "CONFIG_SPIRAM",
        "CONFIG_SPIRAM_MODE",
        "CONFIG_FREERTOS_USE_TRACE_FACILITY",
        "CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS",
    }
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if "=" not in line or line.startswith("#"):
            continue
        key, value = line.split("=", 1)
        if key in interesting:
            budget.sdkconfig_flags[key] = value.strip().strip('"')


def observe_profile_payload(payload: dict[str, Any], budget: Budget) -> None:
    """处理 scripts/resource_budget_report.py 中的 observe profile payload 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    budget.profile_samples += 1
    heap = payload.get("heap") if isinstance(payload.get("heap"), dict) else {}
    budget.observe_min("min_internal_free", safe_int(heap.get("internal_free"), -1))
    budget.observe_min("min_internal_largest", safe_int(heap.get("internal_largest"), -1))
    budget.observe_min("min_psram_free", safe_int(heap.get("psram_free"), -1))
    budget.observe_min("min_psram_largest", safe_int(heap.get("psram_largest"), -1))
    stack = payload.get("stack") if isinstance(payload.get("stack"), dict) else {}
    for value in stack.values():
        budget.observe_min("min_stack_words", safe_int(value, -1))
    events = payload.get("event_bus") if isinstance(payload.get("event_bus"), dict) else {}
    budget.max_event_dropped = max(budget.max_event_dropped, safe_int(events.get("dropped"), 0))
    budget.max_event_maintenance_skipped = max(
        budget.max_event_maintenance_skipped,
        safe_int(events.get("maintenance_skipped"), 0),
    )
    tool_pool = payload.get("tool_pool") if isinstance(payload.get("tool_pool"), dict) else {}
    budget.max_tool_waiting = max(budget.max_tool_waiting, safe_int(tool_pool.get("max_waiting"), 0))


def parse_events_jsonl(path: Path | None, budget: Budget) -> None:
    """处理 scripts/resource_budget_report.py 中的 parse events jsonl 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    if not path or not path.exists():
        return
    with path.open("r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            try:
                row = json.loads(line)
            except json.JSONDecodeError:
                continue
            event = row.get("event") if isinstance(row, dict) else None
            if not isinstance(event, dict) or str(event.get("kind", "")).lower() != "profile":
                continue
            text = event.get("text")
            if not isinstance(text, str):
                continue
            payload = parse_profile_text(text)
            if payload:
                observe_profile_payload(payload, budget)


def parse_summary(path: Path | None, budget: Budget) -> None:
    """处理 scripts/resource_budget_report.py 中的 parse summary 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    summary = read_json(path)
    if not summary:
        return
    for key, attr in [
        ("min_internal_heap", "min_internal_free"),
        ("profile_min_internal_heap", "min_internal_free"),
        ("profile_min_psram_heap", "min_psram_free"),
        ("profile_min_psram_largest", "min_psram_largest"),
        ("profile_min_stack_words", "min_stack_words"),
        ("max_events_dropped", "max_event_dropped"),
    ]:
        value = safe_int(summary.get(key), -1)
        if value < 0:
            continue
        if attr.startswith("max_"):
            setattr(budget, attr, max(getattr(budget, attr), value))
        else:
            budget.observe_min(attr, value)
    budget.ota_last_failure_code = summary.get("ota_last_failure_code") or budget.ota_last_failure_code
    budget.observe_min("ota_min_psram_largest", safe_int(summary.get("ota_min_psram_largest"), -1))
    budget.observe_min("ota_min_internal_largest", safe_int(summary.get("ota_min_internal_largest"), -1))


def evaluate(budget: Budget, thresholds: dict[str, int]) -> None:
    """处理 scripts/resource_budget_report.py 中的 evaluate 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    checks = [
        ("min_internal_largest", "internal_largest_min", "internal RAM largest block below OTA IO threshold"),
        ("min_psram_largest", "psram_largest_min", "PSRAM largest block below OTA staging threshold"),
        ("min_stack_words", "stack_high_water_words_min", "task stack high-water below threshold"),
    ]
    for attr, threshold_key, message in checks:
        value = getattr(budget, attr)
        if value is not None and value < thresholds[threshold_key]:
            budget.issues.append(f"{message}: {value} < {thresholds[threshold_key]}")
    if budget.max_event_dropped > thresholds["event_drop_max"]:
        budget.issues.append(f"event drops observed: {budget.max_event_dropped}")
    if budget.ota_last_failure_code and budget.ota_last_failure_code not in {"none", "n/a"}:
        budget.issues.append(f"last OTA failure code: {budget.ota_last_failure_code}")


def as_dict(budget: Budget) -> dict[str, Any]:
    """处理 scripts/resource_budget_report.py 中的 as dict 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    return {
        "map": {
            "flash_bytes": budget.map_flash_bytes,
            "dram_bytes": budget.map_dram_bytes,
            "iram_bytes": budget.map_iram_bytes,
        },
        "sdkconfig": budget.sdkconfig_flags,
        "runtime": {
            "profile_samples": budget.profile_samples,
            "min_internal_free": budget.min_internal_free,
            "min_internal_largest": budget.min_internal_largest,
            "min_psram_free": budget.min_psram_free,
            "min_psram_largest": budget.min_psram_largest,
            "min_stack_words": budget.min_stack_words,
            "max_event_dropped": budget.max_event_dropped,
            "max_event_maintenance_skipped": budget.max_event_maintenance_skipped,
            "max_tool_waiting": budget.max_tool_waiting,
        },
        "ota": {
            "last_failure_code": budget.ota_last_failure_code,
            "min_psram_largest": budget.ota_min_psram_largest,
            "min_internal_largest": budget.ota_min_internal_largest,
        },
        "issues": budget.issues,
    }


def build_markdown(budget: Budget) -> str:
    """处理 scripts/resource_budget_report.py 中的 build markdown 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    data = as_dict(budget)
    lines = [
        "# C-Claw ESP32S31 Resource Budget",
        "",
        "## Static Artifacts",
        f"- Flash bytes from map: {data['map']['flash_bytes']}",
        f"- DRAM bytes from map: {data['map']['dram_bytes']}",
        f"- IRAM bytes from map: {data['map']['iram_bytes']}",
        "",
        "## Runtime Minima",
        f"- Internal free/largest: {budget.min_internal_free}/{budget.min_internal_largest}",
        f"- PSRAM free/largest: {budget.min_psram_free}/{budget.min_psram_largest}",
        f"- Min task stack high-water words: {budget.min_stack_words}",
        f"- Event drops / maintenance skips: {budget.max_event_dropped}/{budget.max_event_maintenance_skipped}",
        f"- Tool pool max waiting: {budget.max_tool_waiting}",
        "",
        "## OTA",
        f"- Last failure code: {budget.ota_last_failure_code}",
        f"- OTA min PSRAM/internal largest: {budget.ota_min_psram_largest}/{budget.ota_min_internal_largest}",
        "",
        "## sdkconfig Signals",
    ]
    if budget.sdkconfig_flags:
        lines.extend(f"- {key}={value}" for key, value in sorted(budget.sdkconfig_flags.items()))
    else:
        lines.append("- No sdkconfig provided")
    lines.extend(["", "## Issues"])
    lines.extend(f"- {issue}" for issue in budget.issues) if budget.issues else lines.append("- none")
    return "\n".join(lines) + "\n"


def build_parser() -> argparse.ArgumentParser:
    """处理 scripts/resource_budget_report.py 中的 build parser 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    parser = argparse.ArgumentParser(description="Generate a C-Claw ESP32S31 resource budget report")
    parser.add_argument("--map-file", type=Path)
    parser.add_argument("--sdkconfig", type=Path)
    parser.add_argument("--summary-json", type=Path)
    parser.add_argument("--events-jsonl", type=Path)
    parser.add_argument("--out-json", type=Path)
    parser.add_argument("--out-md", type=Path)
    parser.add_argument("--internal-largest-min", type=int, default=DEFAULT_THRESHOLDS["internal_largest_min"])
    parser.add_argument("--psram-largest-min", type=int, default=DEFAULT_THRESHOLDS["psram_largest_min"])
    parser.add_argument("--stack-high-water-words-min", type=int, default=DEFAULT_THRESHOLDS["stack_high_water_words_min"])
    parser.add_argument("--event-drop-max", type=int, default=DEFAULT_THRESHOLDS["event_drop_max"])
    return parser


def main(argv: list[str] | None = None) -> int:
    """处理 scripts/resource_budget_report.py 中的 main 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    args = build_parser().parse_args(argv)
    budget = Budget()
    parse_map_file(args.map_file, budget)
    parse_sdkconfig(args.sdkconfig, budget)
    parse_summary(args.summary_json, budget)
    parse_events_jsonl(args.events_jsonl, budget)
    evaluate(
        budget,
        {
            "internal_largest_min": args.internal_largest_min,
            "psram_largest_min": args.psram_largest_min,
            "stack_high_water_words_min": args.stack_high_water_words_min,
            "event_drop_max": args.event_drop_max,
        },
    )
    if args.out_json:
        args.out_json.parent.mkdir(parents=True, exist_ok=True)
        args.out_json.write_text(json.dumps(as_dict(budget), ensure_ascii=False, indent=2, sort_keys=True), encoding="utf-8")
    if args.out_md:
        args.out_md.parent.mkdir(parents=True, exist_ok=True)
        args.out_md.write_text(build_markdown(budget), encoding="utf-8")
    if not args.out_json and not args.out_md:
        print(build_markdown(budget), end="")
    return 1 if budget.issues else 0


if __name__ == "__main__":
    raise SystemExit(main())
