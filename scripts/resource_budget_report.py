#!/usr/bin/env python3
"""Generate a C-Claw ESP32S31 resource budget report from existing artifacts."""

from __future__ import annotations

import argparse
import json
import math
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
CPU_IDLE_STARVATION_THRESHOLD_PCT = 1.0


def safe_int(value: Any, default: int = 0) -> int:
    """处理 scripts/resource_budget_report.py 中的 safe int 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def valid_percentage(value: Any) -> float | None:
    """只接受有限的 0..100 百分比。"""
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    number = float(value)
    if not math.isfinite(number) or number < 0.0 or number > 100.0:
        return None
    return number


def cpu_frame_is_valid(cpu: dict[str, Any]) -> bool:
    """只接受新版带明确时间窗口的 CPU 快照。"""
    if cpu.get("available") is not True or cpu.get("runtime_stats") is False:
        return False
    if cpu.get("valid") is not True:
        return False
    window_ms = cpu.get("window_ms")
    if (isinstance(window_ms, bool) or not isinstance(window_ms, (int, float)) or
            not math.isfinite(float(window_ms)) or float(window_ms) <= 0.0):
        return False
    return True


def update_cpu_idle_quality(cpu: dict[str, Any], budget: "Budget") -> None:
    """Keep measured low-idle windows separate from missing IDLE counters."""
    raw_idle = cpu.get("core_idle_pct")
    raw_busy = cpu.get("core_pct")
    expected = safe_int(cpu.get("cores"), 0)
    lengths = [len(value) for value in (raw_idle, raw_busy) if isinstance(value, list)]
    if expected <= 0:
        expected = max(lengths, default=0)
    for index in range(expected):
        key = str(index)
        idle = None
        idle_slot_present = isinstance(raw_idle, list) and index < len(raw_idle)
        if idle_slot_present and raw_idle[index] is not None:
            idle = valid_percentage(raw_idle[index])
            if idle is None:
                budget.cpu_invalid_value_count += 1
        else:
            busy = raw_busy[index] if isinstance(raw_busy, list) and index < len(raw_busy) else None
            busy_value = valid_percentage(busy)
            if busy_value is not None:
                idle = 100.0 - busy_value
            elif busy is not None:
                budget.cpu_invalid_value_count += 1
        if idle is None:
            budget.cpu_idle_missing_samples[key] = budget.cpu_idle_missing_samples.get(key, 0) + 1
            budget.cpu_idle_low_streak[key] = 0
        elif idle <= CPU_IDLE_STARVATION_THRESHOLD_PCT:
            budget.cpu_idle_low_samples[key] = budget.cpu_idle_low_samples.get(key, 0) + 1
            streak = budget.cpu_idle_low_streak.get(key, 0) + 1
            budget.cpu_idle_low_streak[key] = streak
            budget.cpu_idle_low_max_streak[key] = max(
                budget.cpu_idle_low_max_streak.get(key, 0), streak
            )
        else:
            budget.cpu_idle_low_streak[key] = 0


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
    cpu_available_samples: int = 0
    cpu_valid_samples: int = 0
    cpu_invalid_samples: int = 0
    cpu_legacy_samples: int = 0
    cpu_invalid_value_count: int = 0
    cpu_truncated_samples: int = 0
    cpu_unknown_core_task_samples: int = 0
    cpu_duplicate_samples: int = 0
    cpu_idle_unavailable_samples: int = 0
    cpu_idle_missing_samples: dict[str, int] = field(default_factory=dict)
    cpu_idle_low_samples: dict[str, int] = field(default_factory=dict)
    cpu_idle_low_streak: dict[str, int] = field(default_factory=dict)
    cpu_idle_low_max_streak: dict[str, int] = field(default_factory=dict)
    cpu_total_samples: int = 0
    cpu_total_weight_ms: float = 0.0
    cpu_total_weighted_pct: float = 0.0
    cpu_total_max_pct: float = 0.0
    cpu_core_weight_ms: dict[str, float] = field(default_factory=dict)
    cpu_core_weighted_pct: dict[str, float] = field(default_factory=dict)
    cpu_core_max_pct: dict[str, float] = field(default_factory=dict)
    cpu_task_names: dict[str, str] = field(default_factory=dict)
    cpu_task_max_pct: dict[str, float] = field(default_factory=dict)
    _seen_cpu_sample_keys: set[int] = field(default_factory=set, repr=False)
    _last_cpu_sample_seq: int | None = field(default=None, repr=False)
    _last_cpu_timestamp_ms: int | None = field(default=None, repr=False)
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
    cpu = payload.get("cpu") if isinstance(payload.get("cpu"), dict) else {}
    if not cpu:
        return
    raw_sample_seq = cpu.get("sample_seq")
    if isinstance(raw_sample_seq, int) and not isinstance(raw_sample_seq, bool) and raw_sample_seq > 0:
        timestamp_ms = safe_int(payload.get("timestamp_ms"), -1)
        if ((budget._last_cpu_sample_seq is not None and
             raw_sample_seq < budget._last_cpu_sample_seq) or
            (budget._last_cpu_timestamp_ms is not None and timestamp_ms >= 0 and
             timestamp_ms < budget._last_cpu_timestamp_ms)):
            budget._seen_cpu_sample_keys.clear()
            budget.cpu_idle_low_streak.clear()
        if raw_sample_seq in budget._seen_cpu_sample_keys:
            budget.cpu_duplicate_samples += 1
            return
        budget._seen_cpu_sample_keys.add(raw_sample_seq)
        budget._last_cpu_sample_seq = raw_sample_seq
        if timestamp_ms >= 0:
            budget._last_cpu_timestamp_ms = timestamp_ms
    available = cpu.get("available") is True
    if available:
        budget.cpu_available_samples += 1
        if "valid" not in cpu or "window_ms" not in cpu:
            budget.cpu_legacy_samples += 1
    if cpu.get("truncated") is True:
        budget.cpu_truncated_samples += 1
    if not cpu_frame_is_valid(cpu):
        if available:
            budget.cpu_invalid_samples += 1
            if cpu.get("reason") == "core_idle_missing":
                budget.cpu_idle_unavailable_samples += 1
        return
    budget.cpu_valid_samples += 1
    update_cpu_idle_quality(cpu, budget)
    raw_window_ms = cpu.get("window_ms")
    window_ms = float(raw_window_ms) if isinstance(raw_window_ms, (int, float)) else 1.0
    if not math.isfinite(window_ms) or window_ms <= 0:
        window_ms = 1.0

    core_values: dict[str, float] = {}
    core_pct = cpu.get("core_pct")
    if isinstance(core_pct, list):
        for index, raw_value in enumerate(core_pct):
            if raw_value is None:
                continue
            value = valid_percentage(raw_value)
            if value is None:
                budget.cpu_invalid_value_count += 1
                continue
            core_values[str(index)] = value
            budget.cpu_core_weight_ms[str(index)] = budget.cpu_core_weight_ms.get(str(index), 0.0) + window_ms
            budget.cpu_core_weighted_pct[str(index)] = (
                budget.cpu_core_weighted_pct.get(str(index), 0.0) + value * window_ms
            )
            budget.cpu_core_max_pct[str(index)] = max(budget.cpu_core_max_pct.get(str(index), 0.0), value)

    total_value = valid_percentage(cpu.get("total_pct"))
    if total_value is None:
        if cpu.get("total_pct") is not None:
            budget.cpu_invalid_value_count += 1
        if core_values:
            total_value = sum(core_values.values()) / len(core_values)
    if total_value is not None:
        budget.cpu_total_samples += 1
        budget.cpu_total_weight_ms += window_ms
        budget.cpu_total_weighted_pct += total_value * window_ms
        budget.cpu_total_max_pct = max(budget.cpu_total_max_pct, total_value)

    tasks = cpu.get("tasks") if isinstance(cpu.get("tasks"), list) else []
    for task in tasks:
        if not isinstance(task, dict):
            continue
        raw_pct = task.get("cpu_pct") if task.get("cpu_pct") is not None else task.get("p")
        raw_core_pct = task.get("core_pct")
        if raw_core_pct is None:
            raw_core_pct = task.get("core_p")
        if raw_core_pct is not None and valid_percentage(raw_core_pct) is None:
            budget.cpu_invalid_value_count += 1
        value = valid_percentage(raw_pct)
        if value is None:
            if raw_pct is not None:
                budget.cpu_invalid_value_count += 1
            continue
        name = str(task.get("name") or task.get("n") or "unknown")
        raw_id = task.get("task_id") if "task_id" in task else task.get("id")
        task_id = safe_int(raw_id, -1) if raw_id is not None and not isinstance(raw_id, bool) else -1
        key = f"id:{task_id}:{name}" if task_id >= 0 else f"name:{name}"
        budget.cpu_task_names[key] = name
        budget.cpu_task_max_pct[key] = max(budget.cpu_task_max_pct.get(key, 0.0), value)
        raw_core = task.get("core") if "core" in task else task.get("c")
        if raw_core is None or (not isinstance(raw_core, bool) and safe_int(raw_core, -1) < 0):
            budget.cpu_unknown_core_task_samples += 1


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


def parse_summary(path: Path | None, budget: Budget, include_cpu: bool = True) -> None:
    """处理 scripts/resource_budget_report.py 中的 parse summary 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    summary = read_json(path)
    if not summary:
        return
    budget.profile_samples = max(budget.profile_samples, safe_int(summary.get("profile_events"), 0))
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
    if not include_cpu:
        return

    quality = summary.get("profile_cpu_quality") if isinstance(summary.get("profile_cpu_quality"), dict) else {}
    budget.cpu_available_samples = max(budget.cpu_available_samples, safe_int(quality.get("available_samples"), 0))
    budget.cpu_valid_samples = max(budget.cpu_valid_samples, safe_int(quality.get("valid_samples"), 0))
    budget.cpu_invalid_samples = max(budget.cpu_invalid_samples, safe_int(quality.get("invalid_samples"), 0))
    budget.cpu_legacy_samples = max(budget.cpu_legacy_samples, safe_int(quality.get("legacy_samples"), 0))
    budget.cpu_invalid_value_count = max(budget.cpu_invalid_value_count, safe_int(quality.get("invalid_value_count"), 0))
    budget.cpu_truncated_samples = max(budget.cpu_truncated_samples, safe_int(quality.get("truncated_samples"), 0))
    budget.cpu_duplicate_samples = max(budget.cpu_duplicate_samples, safe_int(quality.get("duplicate_samples"), 0))
    budget.cpu_idle_unavailable_samples = max(
        budget.cpu_idle_unavailable_samples,
        safe_int(quality.get("idle_unavailable_samples"), 0),
    )
    for attr, key in (
        ("cpu_idle_missing_samples", "idle_missing_samples"),
        ("cpu_idle_low_samples", "idle_low_samples"),
        ("cpu_idle_low_max_streak", "idle_low_max_streak"),
        ("cpu_idle_low_streak", "idle_low_streak"),
    ):
        values = quality.get(key) if isinstance(quality.get(key), dict) else {}
        target = getattr(budget, attr)
        for core, value in values.items():
            target[str(core)] = max(target.get(str(core), 0), safe_int(value, 0))
    budget.cpu_unknown_core_task_samples = max(
        budget.cpu_unknown_core_task_samples,
        safe_int(quality.get("unknown_core_task_samples"), 0),
    )

    total = summary.get("profile_cpu_total") if isinstance(summary.get("profile_cpu_total"), dict) else {}
    total_samples = max(0, safe_int(total.get("samples"), 0))
    total_average = valid_percentage(total.get("avg_pct"))
    total_weight = total.get("weight_ms")
    total_weight = float(total_weight) if isinstance(total_weight, (int, float)) and math.isfinite(float(total_weight)) and total_weight > 0 else float(total_samples)
    budget.cpu_total_samples = max(budget.cpu_total_samples, total_samples)
    if total_average is not None and total_weight > 0:
        budget.cpu_total_weight_ms = max(budget.cpu_total_weight_ms, total_weight)
        budget.cpu_total_weighted_pct = max(budget.cpu_total_weighted_pct, total_average * total_weight)
    total_max = valid_percentage(total.get("max_pct"))
    if total_max is not None:
        budget.cpu_total_max_pct = max(budget.cpu_total_max_pct, total_max)

    cores = summary.get("profile_cpu_cores") if isinstance(summary.get("profile_cpu_cores"), dict) else {}
    for core, values in cores.items():
        if not isinstance(values, dict):
            continue
        average = valid_percentage(values.get("avg_pct"))
        raw_weight = values.get("weight_ms")
        weight = float(raw_weight) if isinstance(raw_weight, (int, float)) and math.isfinite(float(raw_weight)) and raw_weight > 0 else 1.0
        if average is not None:
            budget.cpu_core_weight_ms[str(core)] = max(budget.cpu_core_weight_ms.get(str(core), 0.0), weight)
            budget.cpu_core_weighted_pct[str(core)] = max(
                budget.cpu_core_weighted_pct.get(str(core), 0.0),
                average * weight,
            )
        maximum = valid_percentage(values.get("max_pct"))
        if maximum is not None:
            budget.cpu_core_max_pct[str(core)] = max(budget.cpu_core_max_pct.get(str(core), 0.0), maximum)

    tasks = summary.get("profile_cpu_tasks") if isinstance(summary.get("profile_cpu_tasks"), dict) else {}
    for key, values in tasks.items():
        if not isinstance(values, dict):
            continue
        name = str(values.get("name") or key)
        maximum = valid_percentage(values.get("max_pct"))
        if maximum is None:
            continue
        budget.cpu_task_names[str(key)] = name
        budget.cpu_task_max_pct[str(key)] = max(budget.cpu_task_max_pct.get(str(key), 0.0), maximum)


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
    if budget.cpu_available_samples > 0 and budget.cpu_valid_samples == 0:
        budget.issues.append("CPU profile has no valid sampling window")
    if budget.cpu_legacy_samples > 0:
        budget.issues.append(f"legacy CPU profile frames ignored: {budget.cpu_legacy_samples}")
    if budget.cpu_invalid_value_count > 0:
        budget.issues.append(f"invalid CPU percentage values: {budget.cpu_invalid_value_count}")
    if budget.cpu_idle_unavailable_samples > 0:
        budget.issues.append(
            f"CPU IDLE counters unavailable in {budget.cpu_idle_unavailable_samples} window(s)"
        )
    for core, streak in sorted(budget.cpu_idle_low_max_streak.items()):
        if streak >= 3:
            budget.issues.append(
                f"possible CPU{core} IDLE starvation: {streak} consecutive low-idle windows"
            )


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
            "cpu_available_samples": budget.cpu_available_samples,
            "cpu_valid_samples": budget.cpu_valid_samples,
            "cpu_invalid_samples": budget.cpu_invalid_samples,
            "cpu_legacy_samples": budget.cpu_legacy_samples,
            "cpu_invalid_value_count": budget.cpu_invalid_value_count,
            "cpu_truncated_samples": budget.cpu_truncated_samples,
            "cpu_unknown_core_task_samples": budget.cpu_unknown_core_task_samples,
            "cpu_duplicate_samples": budget.cpu_duplicate_samples,
            "cpu_idle_starvation_threshold_pct": CPU_IDLE_STARVATION_THRESHOLD_PCT,
            "cpu_idle_unavailable_samples": budget.cpu_idle_unavailable_samples,
            "cpu_idle_missing_samples": dict(sorted(budget.cpu_idle_missing_samples.items())),
            "cpu_idle_low_samples": dict(sorted(budget.cpu_idle_low_samples.items())),
            "cpu_idle_low_streak": dict(sorted(budget.cpu_idle_low_streak.items())),
            "cpu_idle_low_max_streak": dict(sorted(budget.cpu_idle_low_max_streak.items())),
            "cpu_total_samples": budget.cpu_total_samples,
            "cpu_total_avg_pct": (
                round(budget.cpu_total_weighted_pct / budget.cpu_total_weight_ms, 2)
                if budget.cpu_total_weight_ms > 0 else None
            ),
            "cpu_total_max_pct": round(budget.cpu_total_max_pct, 2),
            "cpu_cores": {
                core: {
                    "avg_pct": round(budget.cpu_core_weighted_pct[core] / budget.cpu_core_weight_ms[core], 2),
                    "max_pct": round(budget.cpu_core_max_pct.get(core, 0.0), 2),
                }
                for core in sorted(budget.cpu_core_weight_ms)
                if budget.cpu_core_weight_ms.get(core, 0.0) > 0
            },
            "cpu_top_tasks": [
                {
                    "name": budget.cpu_task_names.get(key, key),
                    "key": key,
                    "max_pct": round(value, 2),
                }
                for key, value in sorted(budget.cpu_task_max_pct.items(), key=lambda item: item[1], reverse=True)[:12]
            ],
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
        f"- CPU valid/invalid/legacy windows: {budget.cpu_valid_samples}/{budget.cpu_invalid_samples}/{budget.cpu_legacy_samples} (available={budget.cpu_available_samples})",
        f"- CPU total window-weighted avg/max: {data['runtime']['cpu_total_avg_pct']}/{data['runtime']['cpu_total_max_pct']}%",
        f"- CPU truncated frames / unbound-task rows / duplicate windows: {budget.cpu_truncated_samples}/{budget.cpu_unknown_core_task_samples}/{budget.cpu_duplicate_samples}",
        f"- CPU IDLE unavailable windows / low-idle samples / max streak: {budget.cpu_idle_unavailable_samples}/{budget.cpu_idle_low_samples}/{budget.cpu_idle_low_max_streak}",
        "- CPU core averages/maxima: " + (
            ", ".join(
                f"CPU{core}={values['avg_pct']}/{values['max_pct']}%"
                for core, values in data["runtime"]["cpu_cores"].items()
            ) or "n/a"
        ),
        "- CPU top task maxima: " + (
            ", ".join(
                f"{item['name']}={item['max_pct']}%"
                for item in data["runtime"]["cpu_top_tasks"]
            ) or "n/a"
        ),
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
    parse_summary(
        args.summary_json,
        budget,
        include_cpu=not (args.events_jsonl is not None and args.events_jsonl.exists()),
    )
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
