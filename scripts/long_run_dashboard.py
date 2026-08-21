#!/usr/bin/env python3
"""Local Web dashboard for C-Claw long-run tests."""

from __future__ import annotations

import argparse
import json
import mimetypes
import os
import re
import subprocess
import sys
import threading
import time
from datetime import datetime, timezone
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib import parse


ROOT = Path(__file__).resolve().parents[1]
LONG_RUN_SCRIPT = ROOT / "scripts" / "long_run_test.py"
DEFAULT_HOST = "http://cclaw.local/"
DEFAULT_SERIAL_PORT = "COM6"
DEFAULT_BAUDRATE = 115200
DEFAULT_OUTPUT_ROOT = ROOT / "outputs" / "long_run_dashboard"
ALL_TEST_ITEMS = [
    "status",
    "events",
    "profile",
    "ota_status",
    "config_audit",
    "fault_ota_reject",
    "fault_web_timeout",
    "network_commands",
    "serial_commands",
    "short_conversation",
    "long_conversation",
]


def utc_now() -> str:
    """处理 scripts/long_run_dashboard.py 中的 utc now 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")


def parse_utc(value: Any) -> datetime | None:
    """处理 scripts/long_run_dashboard.py 中的 parse utc 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    if not isinstance(value, str) or not value:
        return None
    try:
        return datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        return None


def make_run_id() -> str:
    """处理 scripts/long_run_dashboard.py 中的 make run id 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    return datetime.now().strftime("run_%Y%m%d-%H%M%S-%f")[:-3]


def compact_text(text: str, limit: int = 400) -> str:
    """处理 scripts/long_run_dashboard.py 中的 compact text 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    one_line = " ".join((text or "").split())
    return one_line if len(one_line) <= limit else one_line[: limit - 3] + "..."


def read_json(path: Path) -> dict[str, Any] | None:
    """处理 scripts/long_run_dashboard.py 中的 read json 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return None
    return value if isinstance(value, dict) else None


def read_tail(path: Path, max_lines: int = 80) -> list[str]:
    """处理 scripts/long_run_dashboard.py 中的 read tail 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    if not path.exists():
        return []
    try:
        with path.open("r", encoding="utf-8", errors="replace") as handle:
            lines = handle.readlines()
    except OSError:
        return []
    return [line.rstrip("\r\n") for line in lines[-max_lines:]]


def newest_jsonl_row(path: Path) -> dict[str, Any] | None:
    """处理 scripts/long_run_dashboard.py 中的 newest jsonl row 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    for line in reversed(read_tail(path, 120)):
        try:
            value = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(value, dict):
            return value
    return None


def profile_snapshot_from_text(text: str) -> dict[str, Any]:
    """处理 scripts/long_run_dashboard.py 中的 profile snapshot from text 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    try:
        payload = json.loads(text)
    except json.JSONDecodeError:
        payload = None
    if isinstance(payload, dict):
        cpu = payload.get("cpu") if isinstance(payload.get("cpu"), dict) else {}
        heap = payload.get("heap") if isinstance(payload.get("heap"), dict) else {}
        return {
            "cpu": cpu,
            "tasks": [task for task in cpu.get("tasks", []) if isinstance(task, dict)],
            "heap": heap,
            "timestamp_ms": payload.get("timestamp_ms"),
        }

    tasks: list[dict[str, Any]] = []
    verbose_pattern = re.compile(
        r'\{"name":"(?P<name>[^"]+)".*?"cpu_pct":(?P<pct>[0-9]+(?:\.[0-9]+)?)'
    )
    compact_pattern = re.compile(
        r'\{"n":"(?P<name>[^"]+)".*?"p":(?P<pct>[0-9]+(?:\.[0-9]+)?)'
    )
    for pattern in (verbose_pattern, compact_pattern):
        for match in pattern.finditer(text):
            try:
                raw = match.group(0)
                task: dict[str, Any] = {"n": match.group("name"), "p": float(match.group("pct"))}
                core_match = re.search(r'"c"\s*:\s*(-?\d+)', raw)
                if core_match:
                    task["c"] = int(core_match.group(1))
                cp_match = re.search(r'"cp"\s*:\s*([0-9]+(?:\.[0-9]+)?)', raw)
                if cp_match:
                    task["cp"] = float(cp_match.group(1))
                tasks.append(task)
            except ValueError:
                continue
    heap: dict[str, Any] = {}
    for key in ("internal_free", "internal_total", "internal_largest", "psram_free", "psram_total", "psram_largest", "free", "min_free"):
        match = re.search(rf'"{key}"\s*:\s*(\d+)', text)
        if match:
            heap[key] = int(match.group(1))
    total_pct = None
    match = re.search(r'"total_pct"\s*:\s*([0-9]+(?:\.[0-9]+)?)', text)
    if match:
        total_pct = float(match.group(1))
    cores = None
    match = re.search(r'"cores"\s*:\s*(\d+)', text)
    if match:
        cores = int(match.group(1))
    core_pct = None
    match = re.search(r'"core_pct"\s*:\s*\[([^\]]*)', text)
    if match:
        core_pct = []
        for part in match.group(1).split(","):
            try:
                core_pct.append(float(part.strip()))
            except ValueError:
                pass
    return {
        "cpu": {"total_pct": total_pct, "cores": cores, "core_pct": core_pct},
        "tasks": tasks,
        "heap": heap,
        "timestamp_ms": None,
    }


def cpu_task_value(task: dict[str, Any]) -> tuple[str, float, int | None] | None:
    """处理 scripts/long_run_dashboard.py 中的 cpu task value 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    name = str(task.get("name") or task.get("n") or "unknown")
    pct = task.get("core_pct")
    if pct is None:
        pct = task.get("cp")
    if pct is None:
        pct = task.get("cpu_pct")
    if pct is None:
        pct = task.get("p")
    if not isinstance(pct, (int, float)):
        return None
    core = task.get("core")
    if core is None:
        core = task.get("c")
    core_id = int(core) if isinstance(core, int) and core >= 0 else None
    if core_id is None and name.upper().startswith("IDLE") and name[-1:].isdigit():
        core_id = int(name[-1])
    return name, round(max(0.0, min(100.0, float(pct))), 2), core_id


def core_totals_from_cpu(cpu: dict[str, Any], core_samples: dict[int, dict[str, float]]) -> dict[int, float | None]:
    """处理 scripts/long_run_dashboard.py 中的 core totals from cpu 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    out: dict[int, float | None] = {}
    core_pct = cpu.get("core_pct")
    if isinstance(core_pct, list):
        for idx, value in enumerate(core_pct):
            if isinstance(value, (int, float)):
                out[idx] = round(max(0.0, min(100.0, float(value))), 2)
    for core_id, sample in core_samples.items():
        if core_id in out:
            continue
        idle_name = f"IDLE{core_id}"
        idle = sample.get(idle_name)
        out[core_id] = None if idle is None else round(max(0.0, min(100.0, 100.0 - idle)), 2)
    return out


def read_cpu_series(path: Path, max_lines: int = 900, max_points: int = 160, max_tasks: int = 6) -> dict[str, Any]:
    """处理 scripts/long_run_dashboard.py 中的 read cpu series 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    core_rows: dict[int, list[dict[str, Any]]] = {}
    task_max: dict[int, dict[str, float]] = {}
    task_last: dict[int, dict[str, float]] = {}
    for line in read_tail(path, max_lines):
        try:
            row = json.loads(line)
        except json.JSONDecodeError:
            continue
        event = row.get("event") if isinstance(row, dict) else None
        if not isinstance(event, dict) or str(event.get("kind", "")).lower() != "profile":
            continue
        text = event.get("text")
        if not isinstance(text, str) or "\"cpu\"" not in text:
            continue
        snapshot = profile_snapshot_from_text(text)
        tasks = snapshot["tasks"]
        if not tasks:
            continue
        core_samples: dict[int, dict[str, float]] = {}
        for task in tasks:
            if not isinstance(task, dict):
                continue
            parsed = cpu_task_value(task)
            if parsed is None:
                continue
            name, value, core_id = parsed
            if core_id is None:
                continue
            sample = core_samples.setdefault(core_id, {})
            sample[name] = value
            task_max.setdefault(core_id, {})[name] = max(task_max.setdefault(core_id, {}).get(name, 0.0), value)
            task_last.setdefault(core_id, {})[name] = value
        core_totals = core_totals_from_cpu(snapshot["cpu"], core_samples)
        timestamp = snapshot.get("timestamp_ms") or event.get("time_ms")
        for core_id, sample in core_samples.items():
            core_rows.setdefault(core_id, []).append(
                {
                    "timestamp_ms": timestamp,
                    "total_pct": core_totals.get(core_id),
                    "tasks": sample,
                }
            )
    cores: dict[str, Any] = {}
    for core_id in sorted(core_rows):
        rows = core_rows[core_id][-max_points:]
        max_for_core = task_max.get(core_id, {})
        last_for_core = task_last.get(core_id, {})
        top_tasks = sorted(max_for_core, key=lambda name: max_for_core.get(name, 0.0), reverse=True)[:max_tasks]
        cores[str(core_id)] = {
            "points": rows,
            "top_tasks": top_tasks,
            "task_max_pct": {name: round(max_for_core.get(name, 0.0), 2) for name in top_tasks},
            "task_last_pct": {name: round(last_for_core.get(name, 0.0), 2) for name in top_tasks},
        }
    return {"cores": cores}


def read_memory_series(path: Path, max_lines: int = 900, max_points: int = 160) -> dict[str, Any]:
    """处理 scripts/long_run_dashboard.py 中的 read memory series 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    rows: list[dict[str, Any]] = []
    last: dict[str, float] = {}
    maxs: dict[str, float] = {}
    for line in read_tail(path, max_lines):
        try:
            row = json.loads(line)
        except json.JSONDecodeError:
            continue
        event = row.get("event") if isinstance(row, dict) else None
        if not isinstance(event, dict) or str(event.get("kind", "")).lower() != "profile":
            continue
        text = event.get("text")
        if not isinstance(text, str) or "\"heap\"" not in text:
            continue
        snapshot = profile_snapshot_from_text(text)
        heap = snapshot.get("heap") if isinstance(snapshot.get("heap"), dict) else {}
        sample: dict[str, float] = {}
        internal_free = heap.get("internal_free")
        internal_total = heap.get("internal_total")
        psram_free = heap.get("psram_free")
        psram_total = heap.get("psram_total")
        psram_largest = heap.get("psram_largest")
        if isinstance(internal_free, int) and isinstance(internal_total, int) and internal_total > 0:
            sample["internal_used_pct"] = round(max(0.0, min(100.0, (internal_total - internal_free) * 100.0 / internal_total)), 2)
        if isinstance(psram_free, int) and isinstance(psram_total, int) and psram_total > 0:
            sample["psram_used_pct"] = round(max(0.0, min(100.0, (psram_total - psram_free) * 100.0 / psram_total)), 2)
        if isinstance(psram_largest, int) and isinstance(psram_total, int) and psram_total > 0:
            sample["psram_fragment_pct"] = round(max(0.0, min(100.0, (psram_free - psram_largest) * 100.0 / psram_total)), 2) if isinstance(psram_free, int) else 0.0
        for key, value in sample.items():
            last[key] = value
            maxs[key] = max(maxs.get(key, 0.0), value)
        if sample:
            rows.append({"timestamp_ms": snapshot.get("timestamp_ms") or event.get("time_ms"), **sample})
    rows = rows[-max_points:]
    return {"points": rows, "last": last, "max": maxs}


class RunManager:
    def __init__(self, output_root: Path):
        """处理 scripts/long_run_dashboard.py 中的 init 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        self.output_root = output_root
        self.output_root.mkdir(parents=True, exist_ok=True)
        self.lock = threading.Lock()
        self.active: dict[str, Any] | None = None

    def list_runs(self) -> list[dict[str, Any]]:
        """处理 scripts/long_run_dashboard.py 中的 list runs 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        rows: list[dict[str, Any]] = []
        if not self.output_root.exists():
            return rows
        active = self.active_run()
        for path in sorted(self.output_root.iterdir(), key=lambda p: p.name, reverse=True):
            if not path.is_dir():
                continue
            summary = read_json(path / "summary.json") or {}
            rows.append(
                {
                    "run_id": path.name,
                    "output_dir": str(path),
                    "run_label": summary.get("run_label"),
                    "started_at": summary.get("started_at"),
                    "finished_at": summary.get("finished_at"),
                    "stop_reason": summary.get("stop_reason"),
                    "incomplete_run": summary.get("incomplete_run"),
                    "active": active is not None and active.get("run_id") == path.name and active.get("returncode") is None,
                }
            )
        return rows

    def active_run(self) -> dict[str, Any] | None:
        """处理 scripts/long_run_dashboard.py 中的 active run 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        with self.lock:
            if self.active and self.active["process"].poll() is not None:
                self.active["returncode"] = self.active["process"].returncode
                log = self.active.get("runner_log")
                if log:
                    try:
                        log.close()
                    except Exception:
                        pass
                    self.active["runner_log"] = None
            if not self.active:
                return None
            return {key: value for key, value in self.active.items() if key not in {"process", "runner_log"}}

    def start_run(self, config: dict[str, Any]) -> dict[str, Any]:
        """处理 scripts/long_run_dashboard.py 中的 start run 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        with self.lock:
            if self.active and self.active["process"].poll() is None:
                raise RuntimeError("a run is already active")

            run_id = make_run_id()
            stop_file = self.output_root / f".{run_id}.stop"
            if stop_file.exists():
                stop_file.unlink()

            duration = str(config.get("duration") or "3h")
            host = str(config.get("host") or DEFAULT_HOST)
            serial_port = str(config.get("serial_port") or DEFAULT_SERIAL_PORT)
            baudrate = int(config.get("baudrate") or DEFAULT_BAUDRATE)
            test_items = config.get("test_items") or ALL_TEST_ITEMS
            if not isinstance(test_items, list):
                test_items = ALL_TEST_ITEMS
            test_items = [item for item in test_items if item in ALL_TEST_ITEMS]
            if not test_items:
                test_items = ["status", "events", "profile"]

            cmd = [
                sys.executable,
                str(LONG_RUN_SCRIPT),
                "--host",
                host,
                "--serial-port",
                serial_port,
                "--baudrate",
                str(baudrate),
                "--duration",
                duration,
                "--output-dir",
                str(self.output_root),
                "--run-id",
                run_id,
                "--stop-file",
                str(stop_file),
                "--test-items",
                ",".join(test_items),
                "--profile-level",
                str(config.get("profile_level") or "basic"),
                "--profile-interval-ms",
                str(int(config.get("profile_interval_ms") or 5000)),
                "--interval",
                str(float(config.get("interval") or 5.0)),
                "--conversation-interval",
                str(float(config.get("conversation_interval") or 100.0)),
                "--long-conversation-interval",
                str(float(config.get("long_conversation_interval") or 300.0)),
                "--long-conversation-turns",
                str(int(config.get("long_conversation_turns") or 8)),
                "--event-poll-interval",
                str(float(config.get("event_poll_interval") or 2.0)),
                "--http-timeout",
                str(float(config.get("http_timeout") or 8.0)),
                "--run-label",
                str(config.get("run_label") or ""),
            ]
            if config.get("no_serial"):
                cmd.append("--no-serial")
            if config.get("dry_run"):
                cmd.append("--dry-run")
            if config.get("no_profile_guard"):
                cmd.append("--no-profile-guard")
            if config.get("no_persistent_http"):
                cmd.append("--no-persistent-http")

            run_dir = self.output_root / run_id
            run_dir.mkdir(parents=True, exist_ok=True)
            runner_log = (run_dir / "runner.log").open("a", encoding="utf-8", buffering=1)
            env = os.environ.copy()
            env.setdefault("PYTHONIOENCODING", "utf-8")
            env.setdefault("PYTHONUTF8", "1")
            process = subprocess.Popen(
                cmd,
                cwd=str(ROOT),
                stdout=runner_log,
                stderr=runner_log,
                env=env,
            )
            run = {
                "run_id": run_id,
                "output_dir": str(run_dir),
                "stop_file": str(stop_file),
                "command": cmd,
                "started_at": utc_now(),
                "process": process,
                "runner_log": runner_log,
            }
            self.active = run
            return {key: value for key, value in run.items() if key not in {"process", "runner_log"}}

    def stop_run(self, run_id: str) -> dict[str, Any]:
        """处理 scripts/long_run_dashboard.py 中的 stop run 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        with self.lock:
            if not self.active or self.active.get("run_id") != run_id:
                raise RuntimeError("run is not active")
            Path(self.active["stop_file"]).write_text("stop\n", encoding="utf-8")
            return {"run_id": run_id, "stop_requested": True}

    def run_status(self, run_id: str) -> dict[str, Any]:
        """处理 scripts/long_run_dashboard.py 中的 run status 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        output_dir = self.output_root / run_id
        summary = read_json(output_dir / "summary.json") or {}
        latest_row = newest_jsonl_row(output_dir / "run_status.jsonl")
        serial_tail = read_tail(output_dir / "serial.log", 40)
        runner_tail = read_tail(output_dir / "runner.log", 80)
        report_exists = (output_dir / "summary_report.txt").exists()
        active = self.active_run()
        active_match = active is not None and active.get("run_id") == run_id and active.get("returncode") is None
        started_at = parse_utc(summary.get("started_at")) or parse_utc(active.get("started_at") if active else None)
        finished_at = parse_utc(summary.get("finished_at"))
        now = datetime.now(timezone.utc)
        if started_at:
            end_at = now if active_match else (finished_at or now)
            live_elapsed_seconds = max(0.0, (end_at - started_at).total_seconds())
        else:
            live_elapsed_seconds = float(summary.get("elapsed_seconds") or 0.0)
        requested_duration_seconds = float(summary.get("requested_duration_seconds") or 0.0)
        remaining_seconds = max(0.0, requested_duration_seconds - live_elapsed_seconds) if requested_duration_seconds > 0 else None
        progress_percent = min(100.0, live_elapsed_seconds * 100.0 / requested_duration_seconds) if requested_duration_seconds > 0 else None
        return {
            "run_id": run_id,
            "output_dir": str(output_dir),
            "exists": output_dir.exists(),
            "active": bool(active_match),
            "live_elapsed_seconds": round(live_elapsed_seconds, 3),
            "requested_duration_seconds": requested_duration_seconds,
            "remaining_seconds": round(remaining_seconds, 3) if remaining_seconds is not None else None,
            "progress_percent": round(progress_percent, 2) if progress_percent is not None else None,
            "summary": summary,
            "latest_row": latest_row,
            "cpu_series": read_cpu_series(output_dir / "events.jsonl"),
            "memory_series": read_memory_series(output_dir / "events.jsonl"),
            "serial_tail": serial_tail[-20:],
            "runner_tail": runner_tail[-40:],
            "artifacts": {
                "summary_report.txt": report_exists,
                "summary.json": (output_dir / "summary.json").exists(),
                "run_status.jsonl": (output_dir / "run_status.jsonl").exists(),
                "events.jsonl": (output_dir / "events.jsonl").exists(),
                "serial.log": (output_dir / "serial.log").exists(),
                "conversation.jsonl": (output_dir / "conversation.jsonl").exists(),
                "long_conversation.jsonl": (output_dir / "long_conversation.jsonl").exists(),
                "runner.log": (output_dir / "runner.log").exists(),
            },
        }


INDEX_HTML = r"""<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>C-Claw 长测控制台</title>
  <style>
    :root { color-scheme: light; --bg:#f6f7f9; --panel:#fff; --text:#17202a; --muted:#667085; --line:#d8dee8; --accent:#0f766e; --danger:#b42318; }
    * { box-sizing: border-box; }
    body { margin:0; font:14px/1.4 system-ui, -apple-system, Segoe UI, sans-serif; color:var(--text); background:var(--bg); }
    header { height:56px; display:flex; align-items:center; justify-content:space-between; padding:0 20px; border-bottom:1px solid var(--line); background:#fff; }
    h1 { margin:0; font-size:18px; font-weight:650; letter-spacing:0; }
    main { display:grid; grid-template-columns:360px 1fr; gap:16px; padding:16px; max-width:1480px; margin:0 auto; }
    section, aside { background:var(--panel); border:1px solid var(--line); border-radius:8px; padding:14px; }
    h2 { margin:0 0 12px; font-size:15px; }
    label { display:block; color:var(--muted); font-size:12px; margin:10px 0 4px; }
    input, select { width:100%; height:34px; border:1px solid var(--line); border-radius:6px; padding:0 9px; background:#fff; color:var(--text); }
    .grid { display:grid; grid-template-columns:1fr 1fr; gap:8px; }
    .checks { display:grid; grid-template-columns:1fr; gap:6px; margin:8px 0 12px; }
    .check { display:flex; align-items:center; gap:8px; color:var(--text); font-size:13px; }
    .check input { width:16px; height:16px; }
    button { height:34px; border:1px solid var(--line); border-radius:6px; padding:0 12px; background:#fff; cursor:pointer; }
    button.primary { background:var(--accent); color:#fff; border-color:var(--accent); }
    button.danger { background:var(--danger); color:#fff; border-color:var(--danger); }
    button:disabled { opacity:.5; cursor:not-allowed; }
    .actions { display:flex; gap:8px; margin-top:12px; }
    .cards { display:grid; grid-template-columns:repeat(4,minmax(150px,1fr)); gap:10px; }
    .card { border:1px solid var(--line); border-radius:8px; padding:10px; min-height:72px; background:#fbfcfe; }
    .card .k { color:var(--muted); font-size:12px; }
    .card .v { margin-top:6px; font-size:19px; font-weight:650; overflow-wrap:anywhere; }
    .progress { height:10px; border-radius:999px; background:#e6eaf0; overflow:hidden; margin:2px 0 12px; }
    .progress div { height:100%; width:0%; background:var(--accent); transition:width .25s ease; }
    .chartPanel { margin-top:12px; border:1px solid var(--line); border-radius:8px; padding:10px; background:#fbfcfe; }
    .chartHead { display:flex; align-items:center; justify-content:space-between; gap:8px; margin-bottom:8px; }
    .legend { display:flex; flex-wrap:wrap; gap:8px; color:var(--muted); font-size:12px; }
    .legend span { display:inline-flex; align-items:center; gap:5px; }
    .swatch { width:10px; height:10px; border-radius:2px; display:inline-block; }
    canvas { width:100%; height:260px; display:block; }
    .cols { display:grid; grid-template-columns:1fr 1fr; gap:12px; margin-top:12px; }
    pre { margin:0; padding:10px; min-height:220px; max-height:420px; overflow:auto; background:#111827; color:#e5e7eb; border-radius:8px; font-size:12px; white-space:pre-wrap; }
    .runs { margin-top:12px; display:grid; gap:6px; }
    .run { border:1px solid var(--line); border-radius:6px; padding:8px; cursor:pointer; background:#fff; }
    .run.active { border-color:var(--accent); }
    .links { display:flex; flex-wrap:wrap; gap:8px; margin-top:10px; }
    .links a { color:var(--accent); text-decoration:none; border:1px solid var(--line); border-radius:6px; padding:6px 8px; background:#fff; }
    .muted { color:var(--muted); }
    @media (max-width: 980px) { main { grid-template-columns:1fr; } .cards { grid-template-columns:1fr 1fr; } .cols { grid-template-columns:1fr; } }
  </style>
</head>
<body>
<header>
  <h1>C-Claw 长测控制台</h1>
  <div class="muted" id="activeLine">当前没有运行中的测试</div>
</header>
<main>
  <aside>
    <h2>测试配置</h2>
    <label>测试名称</label><input id="run_label" placeholder="1小时压力测试">
    <label>设备地址</label><input id="host">
    <div class="grid">
      <div><label>串口</label><input id="serial_port"></div>
      <div><label>波特率</label><input id="baudrate" type="number"></div>
    </div>
    <div class="grid">
      <div><label>测试时长</label><input id="duration" value="1h"></div>
      <div><label>循环间隔（秒）</label><input id="interval" type="number" value="5"></div>
    </div>
    <div class="grid">
      <div><label>Profile 等级</label><select id="profile_level"><option>trace</option><option>trace</option><option>keep</option></select></div>
      <div><label>Profile 间隔（毫秒）</label><input id="profile_interval_ms" type="number" value="5000"></div>
    </div>
    <div class="grid">
      <div><label>短对话间隔（秒）</label><input id="conversation_interval" type="number" value="100"></div>
      <div><label>长对话间隔（秒）</label><input id="long_conversation_interval" type="number" value="300"></div>
    </div>
    <div class="grid">
      <div><label>长对话轮数</label><input id="long_conversation_turns" type="number" value="8"></div>
      <div><label>HTTP 超时（秒）</label><input id="http_timeout" type="number" value="8"></div>
    </div>
    <label>测试项目</label>
    <div class="checks" id="testItems"></div>
    <div class="checks">
      <label class="check"><input id="dry_run" type="checkbox"> 只做 dry-run</label>
      <label class="check"><input id="no_serial" type="checkbox"> 禁用串口</label>
      <label class="check"><input id="no_profile_guard" type="checkbox"> 不自动修改 profile 设置</label>
    </div>
    <div class="actions">
      <button class="primary" id="startBtn">开始测试</button>
      <button class="danger" id="stopBtn" disabled>停止测试</button>
      <button id="refreshBtn">刷新</button>
    </div>
    <h2 style="margin-top:18px">历史记录</h2>
    <div class="runs" id="runs"></div>
  </aside>
  <section>
    <h2>实时状态</h2>
    <div class="progress"><div id="progressBar"></div></div>
    <div class="cards" id="cards"></div>
    <div class="chartPanel">
      <div class="chartHead">
        <h2 style="margin:0">CPU0 百分比占用</h2>
        <div class="muted" id="cpuChartNote">等待 profile CPU 数据</div>
      </div>
      <canvas id="cpuChart0" width="980" height="240"></canvas>
      <div class="legend" id="cpuLegend0"></div>
      <div class="chartHead" style="margin-top:12px">
        <h2 style="margin:0">CPU1 百分比占用</h2>
        <div class="muted" id="cpuChartNote1">等待 profile CPU 数据</div>
      </div>
      <canvas id="cpuChart1" width="980" height="240"></canvas>
      <div class="legend" id="cpuLegend1"></div>
    </div>
    <div class="chartPanel">
      <div class="chartHead">
        <h2 style="margin:0">内存占用</h2>
        <div class="muted" id="memoryChartNote">等待 profile 内存数据</div>
      </div>
      <canvas id="memoryChart" width="980" height="260"></canvas>
      <div class="legend" id="memoryLegend"></div>
    </div>
    <div class="links" id="artifactLinks"></div>
    <div class="cols">
      <div><h2>最新摘要</h2><pre id="summaryBox"></pre></div>
      <div><h2>串口日志尾部</h2><pre id="serialBox"></pre></div>
    </div>
  </section>
</main>
<script>
const itemLabels = {
  status: "状态轮询",
  events: "事件日志",
  profile: "性能与资源指标",
  ota_status: "OTA 状态",
  config_audit: "配置风险审计",
  fault_ota_reject: "OTA 拒绝探针",
  fault_web_timeout: "Web 超时探针",
  network_commands: "网络命令",
  serial_commands: "串口命令",
  short_conversation: "短对话",
  long_conversation: "长对话"
};
let selectedRun = null;
let eventSource = null;

function el(id){ return document.getElementById(id); }
function value(id){ return el(id).value; }
function checked(id){ return el(id).checked; }
async function api(path, opts){ const r = await fetch(path, opts); if(!r.ok) throw new Error(await r.text()); return r.json(); }

function formatDuration(seconds){
  seconds = Math.max(0, Math.round(Number(seconds) || 0));
  const h = Math.floor(seconds / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  const s = seconds % 60;
  if (h > 0) return `${h}时${String(m).padStart(2,"0")}分${String(s).padStart(2,"0")}秒`;
  if (m > 0) return `${m}分${String(s).padStart(2,"0")}秒`;
  return `${s}秒`;
}
function renderItems(items){
  el("testItems").innerHTML = items.map(item => `<label class="check"><input type="checkbox" data-item="${item}" checked> ${itemLabels[item] || item}</label>`).join("");
}
function selectedItems(){
  return [...document.querySelectorAll("[data-item]")].filter(i => i.checked).map(i => i.dataset.item);
}
function config(){
  return {
    run_label: value("run_label"),
    host: value("host"),
    serial_port: value("serial_port"),
    baudrate: Number(value("baudrate")),
    duration: value("duration"),
    interval: Number(value("interval")),
    profile_level: value("profile_level"),
    profile_interval_ms: Number(value("profile_interval_ms")),
    conversation_interval: Number(value("conversation_interval")),
    long_conversation_interval: Number(value("long_conversation_interval")),
    long_conversation_turns: Number(value("long_conversation_turns")),
    http_timeout: Number(value("http_timeout")),
    test_items: selectedItems(),
    dry_run: checked("dry_run"),
    no_serial: checked("no_serial"),
    no_profile_guard: checked("no_profile_guard")
  };
}
function card(k,v){ return `<div class="card"><div class="k">${k}</div><div class="v">${v ?? "n/a"}</div></div>`; }
function metric(summary, path, fallback="n/a"){
  let cur = summary || {};
  for (const part of path.split(".")) { cur = cur && cur[part]; }
  return cur ?? fallback;
}
const cpuColors = ["#111827","#0f766e","#2563eb","#c2410c","#7c3aed","#be123c","#15803d"];
const memColors = {
  internal_free: "#2563eb",
  internal_largest: "#60a5fa",
  psram_free: "#0f766e",
  psram_largest: "#34d399"
};
const memLabels = {
  internal_free: "Internal RAM 可用",
  internal_largest: "Internal RAM 最大块",
  psram_free: "PSRAM 可用",
  psram_largest: "PSRAM 最大块"
};
function resizeCanvas(canvas){
  const rect = canvas.getBoundingClientRect();
  const ratio = window.devicePixelRatio || 1;
  const width = Math.max(320, Math.floor(rect.width * ratio));
  const height = Math.max(180, Math.floor(rect.height * ratio));
  if (canvas.width !== width || canvas.height !== height) {
    canvas.width = width;
    canvas.height = height;
  }
  return {width, height, ratio};
}
function drawOneCpuCoreChart(coreId, series){
  const canvas = el(`cpuChart${coreId}`);
  const ctx = canvas.getContext("2d");
  const {width, height, ratio} = resizeCanvas(canvas);
  ctx.clearRect(0, 0, width, height);
  const padL = 44 * ratio, padR = 12 * ratio, padT = 12 * ratio, padB = 28 * ratio;
  const plotW = width - padL - padR;
  const plotH = height - padT - padB;
  const points = (series && series.points) || [];
  const taskNames = (series && series.top_tasks) || [];
  const hasTotal = points.some(p => typeof p.total_pct === "number");
  const lines = hasTotal ? ["__total", ...taskNames] : taskNames;
  const note = el(coreId === 0 ? "cpuChartNote" : "cpuChartNote1");
  const legend = el(`cpuLegend${coreId}`);
  note.textContent = points.length ? `最近 ${points.length} 个 profile 采样` : "等待 profile CPU 数据";
  legend.innerHTML = lines.map((name, i) => {
    if (name === "__total") {
      const lastPoint = [...points].reverse().find(p => typeof p.total_pct === "number");
      const last = lastPoint ? `${lastPoint.total_pct}%` : "n/a";
      return `<span><i class="swatch" style="background:${cpuColors[i % cpuColors.length]}"></i>CPU${coreId} 总占用 ${last}</span>`;
    }
    const last = series.task_last_pct && series.task_last_pct[name] != null ? `${series.task_last_pct[name]}%` : "n/a";
    const max = series.task_max_pct && series.task_max_pct[name] != null ? `${series.task_max_pct[name]}%` : "n/a";
    return `<span><i class="swatch" style="background:${cpuColors[i % cpuColors.length]}"></i>${name} 当前 ${last} / 最大 ${max}</span>`;
  }).join("");
  ctx.font = `${12 * ratio}px system-ui, -apple-system, Segoe UI, sans-serif`;
  ctx.strokeStyle = "#d8dee8";
  ctx.lineWidth = 1 * ratio;
  ctx.fillStyle = "#667085";
  for (let y = 0; y <= 100; y += 25) {
    const py = padT + plotH - (y / 100) * plotH;
    ctx.beginPath();
    ctx.moveTo(padL, py);
    ctx.lineTo(width - padR, py);
    ctx.stroke();
    ctx.fillText(`${y}%`, 6 * ratio, py + 4 * ratio);
  }
  if (!points.length || !lines.length) {
    ctx.fillStyle = "#667085";
    ctx.fillText(`没有 CPU${coreId} 百分比数据：请确认固件 profile 包含 cpu.core_pct 和 task.c/cp`, padL, padT + 32 * ratio);
    return;
  }
  lines.forEach((name, i) => {
    ctx.strokeStyle = cpuColors[i % cpuColors.length];
    ctx.lineWidth = (name === "__total" ? 3 : 2) * ratio;
    ctx.beginPath();
    let drawn = false;
    points.forEach((point, idx) => {
      const value = name === "__total"
        ? (typeof point.total_pct === "number" ? point.total_pct : null)
        : (point.tasks && typeof point.tasks[name] === "number" ? point.tasks[name] : null);
      if (value == null) return;
      const x = padL + (points.length === 1 ? 0 : (idx / (points.length - 1)) * plotW);
      const y = padT + plotH - (Math.max(0, Math.min(100, value)) / 100) * plotH;
      if (!drawn) {
        ctx.moveTo(x, y);
        drawn = true;
      } else {
        ctx.lineTo(x, y);
      }
    });
    if (drawn) ctx.stroke();
  });
  ctx.fillStyle = "#667085";
  ctx.fillText("时间 ->", width - padR - 52 * ratio, height - 8 * ratio);
}
function drawCpuChart(series){
  const cores = (series && series.cores) || {};
  drawOneCpuCoreChart(0, cores["0"] || {});
  drawOneCpuCoreChart(1, cores["1"] || {});
}
function drawMemoryChart(series){
  const canvas = el("memoryChart");
  const ctx = canvas.getContext("2d");
  const {width, height, ratio} = resizeCanvas(canvas);
  ctx.clearRect(0, 0, width, height);
  const padL = 44 * ratio, padR = 12 * ratio, padT = 12 * ratio, padB = 28 * ratio;
  const plotW = width - padL - padR;
  const plotH = height - padT - padB;
  const points = (series && series.points) || [];
  const labels = {
    internal_used_pct: "Internal RAM 占用",
    psram_used_pct: "PSRAM 占用",
    psram_fragment_pct: "PSRAM 碎片/不可连续"
  };
  const colors = {
    internal_used_pct: "#2563eb",
    psram_used_pct: "#0f766e",
    psram_fragment_pct: "#c2410c"
  };
  const keys = Object.keys(labels).filter(k => points.some(p => typeof p[k] === "number"));
  el("memoryChartNote").textContent = points.length ? `最近 ${points.length} 个 profile 采样` : "等待 profile 内存数据";
  el("memoryLegend").innerHTML = keys.map(k => {
    const last = series.last && series.last[k] != null ? `${series.last[k]}%` : "n/a";
    const max = series.max && series.max[k] != null ? `${series.max[k]}%` : "n/a";
    return `<span><i class="swatch" style="background:${colors[k]}"></i>${labels[k]} 当前 ${last} / 最大 ${max}</span>`;
  }).join("");
  ctx.font = `${12 * ratio}px system-ui, -apple-system, Segoe UI, sans-serif`;
  ctx.strokeStyle = "#d8dee8";
  ctx.lineWidth = 1 * ratio;
  ctx.fillStyle = "#667085";
  for (let y = 0; y <= 100; y += 25) {
    const py = padT + plotH - (y / 100) * plotH;
    ctx.beginPath();
    ctx.moveTo(padL, py);
    ctx.lineTo(width - padR, py);
    ctx.stroke();
    ctx.fillText(`${y}%`, 6 * ratio, py + 4 * ratio);
  }
  if (!points.length || !keys.length) {
    ctx.fillStyle = "#667085";
    ctx.fillText("没有内存百分比数据：请确认固件 profile 包含 internal_total 和 psram_total", padL, padT + 32 * ratio);
    return;
  }
  keys.forEach((key) => {
    ctx.strokeStyle = colors[key];
    ctx.lineWidth = 2 * ratio;
    ctx.beginPath();
    let drawn = false;
    points.forEach((point, idx) => {
      const value = typeof point[key] === "number" ? point[key] : null;
      if (value == null) return;
      const x = padL + (points.length === 1 ? 0 : (idx / (points.length - 1)) * plotW);
      const y = padT + plotH - (Math.max(0, Math.min(100, value)) / 100) * plotH;
      if (!drawn) {
        ctx.moveTo(x, y);
        drawn = true;
      } else {
        ctx.lineTo(x, y);
      }
    });
    if (drawn) ctx.stroke();
  });
  ctx.fillStyle = "#667085";
  ctx.fillText("时间 ->", width - padR - 52 * ratio, height - 8 * ratio);
}
function renderStatus(data){
  const s = data.summary || {};
  selectedRun = data.run_id;
  el("activeLine").textContent = data.active ? `运行中：${data.run_id}` : `已选择：${data.run_id}`;
  el("stopBtn").disabled = !data.active;
  const progress = data.progress_percent ?? 0;
  el("progressBar").style.width = `${Math.max(0, Math.min(100, progress))}%`;
  el("cards").innerHTML = [
    card("停止原因", s.stop_reason),
    card("已运行", formatDuration(data.live_elapsed_seconds ?? s.elapsed_seconds)),
    card("目标时长", data.requested_duration_seconds ? formatDuration(data.requested_duration_seconds) : "n/a"),
    card("预计剩余", data.remaining_seconds == null ? "n/a" : formatDuration(data.remaining_seconds)),
    card("HTTP OK/errors", `${s.http_ok || 0}/${s.http_errors || 0}`),
    card("事件序号丢失", s.event_seq_gap_lost || 0),
    card("首 token p95", metric(s, "first_token_summary_ms.p95")),
    card("总耗时 p95", metric(s, "total_elapsed_summary_ms.p95")),
    card("最低 heap", s.profile_min_internal_heap ?? s.min_internal_heap),
    card("Attempt 不一致", s.llm_attempt_consistency_mismatch)
  ].join("");
  drawCpuChart(data.cpu_series || {});
  drawMemoryChart(data.memory_series || {});
  if (Object.keys(s).length || data.latest_row) {
    el("summaryBox").textContent = JSON.stringify({summary:s, latest_row:data.latest_row}, null, 2);
  } else if ((data.runner_tail || []).length) {
    el("summaryBox").textContent = "summary.json 尚未生成。runner.log 尾部：\n\n" + (data.runner_tail || []).join("\n");
  } else {
    el("summaryBox").textContent = "summary.json 尚未生成，runner.log 暂无输出。";
  }
  el("serialBox").textContent = (data.serial_tail || []).join("\n");
  const artifacts = Object.keys(data.artifacts || {}).filter(k => data.artifacts[k]);
  el("artifactLinks").innerHTML = artifacts.map(name => `<a target="_blank" href="/api/runs/${data.run_id}/artifact?name=${encodeURIComponent(name)}">${name}</a>`).join("");
}
async function refreshRuns(){
  const data = await api("/api/runs");
  el("runs").innerHTML = data.runs.map(r => `<div class="run ${r.active ? "active" : ""}" data-run="${r.run_id}"><b>${r.run_id}</b><br><span class="muted">${r.stop_reason || (r.active ? "运行中" : "等待摘要")} ${r.run_label || ""}</span></div>`).join("");
  document.querySelectorAll("[data-run]").forEach(node => node.onclick = () => openRun(node.dataset.run));
  const active = data.runs.find(r => r.active);
  if (active && !selectedRun) openRun(active.run_id);
}
async function refreshSelectedRun(){
  await refreshRuns();
  if (!selectedRun) return;
  const status = await api(`/api/runs/${selectedRun}/status`);
  renderStatus(status);
}
async function openRun(runId){
  selectedRun = runId;
  if (eventSource) eventSource.close();
  eventSource = new EventSource(`/api/runs/${runId}/stream`);
  eventSource.onmessage = ev => renderStatus(JSON.parse(ev.data));
  eventSource.onerror = () => {};
}
async function startRun(){
  const result = await api("/api/runs", {method:"POST", headers:{"Content-Type":"application/json"}, body:JSON.stringify(config())});
  await refreshRuns();
  openRun(result.run_id);
}
async function stopRun(){
  if (!selectedRun) return;
  await api(`/api/runs/${selectedRun}/stop`, {method:"POST"});
}
async function init(){
  const d = await api("/api/defaults");
  el("host").value = d.host;
  el("serial_port").value = d.serial_port;
  el("baudrate").value = d.baudrate;
  renderItems(d.test_items);
  el("startBtn").onclick = startRun;
  el("stopBtn").onclick = stopRun;
  el("refreshBtn").onclick = refreshSelectedRun;
  await refreshRuns();
}
init().catch(err => alert(err.message));
</script>
</body>
</html>
"""


class DashboardHandler(BaseHTTPRequestHandler):
    manager: RunManager

    def log_message(self, fmt: str, *args: Any) -> None:
        """处理 scripts/long_run_dashboard.py 中的 log message 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        sys.stderr.write("[%s] %s\n" % (self.log_date_time_string(), fmt % args))

    def _send_json(self, value: Any, status: int = 200) -> None:
        """处理 scripts/long_run_dashboard.py 中的 send json 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        data = json.dumps(value, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _read_json_body(self) -> dict[str, Any]:
        """处理 scripts/long_run_dashboard.py 中的 read json body 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        length = int(self.headers.get("Content-Length") or "0")
        if length <= 0:
            return {}
        raw = self.rfile.read(length)
        value = json.loads(raw.decode("utf-8"))
        return value if isinstance(value, dict) else {}

    def do_GET(self) -> None:
        """处理 scripts/long_run_dashboard.py 中的 do GET 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        parsed = parse.urlsplit(self.path)
        path = parsed.path
        if path == "/favicon.ico":
            self.send_response(204)
            self.end_headers()
            return
        if path == "/":
            data = INDEX_HTML.encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return
        if path == "/api/defaults":
            self._send_json(
                {
                    "host": DEFAULT_HOST,
                    "serial_port": DEFAULT_SERIAL_PORT,
                    "baudrate": DEFAULT_BAUDRATE,
                    "test_items": ALL_TEST_ITEMS,
                    "default_output_root": str(self.manager.output_root),
                }
            )
            return
        if path == "/api/runs":
            self._send_json({"runs": self.manager.list_runs()})
            return
        parts = [part for part in path.split("/") if part]
        if len(parts) >= 3 and parts[0] == "api" and parts[1] == "runs":
            run_id = parts[2]
            if len(parts) == 4 and parts[3] == "stream":
                self._stream_run(run_id)
                return
            if len(parts) == 4 and parts[3] == "artifact":
                self._send_artifact(run_id, parse.parse_qs(parsed.query).get("name", [""])[0])
                return
            if len(parts) == 4 and parts[3] == "status":
                self._send_json(self.manager.run_status(run_id))
                return
        self._send_json({"error": "未找到"}, HTTPStatus.NOT_FOUND)

    def do_POST(self) -> None:
        """处理 scripts/long_run_dashboard.py 中的 do POST 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        parsed = parse.urlsplit(self.path)
        path = parsed.path
        try:
            if path == "/api/runs":
                self._send_json(self.manager.start_run(self._read_json_body()))
                return
            parts = [part for part in path.split("/") if part]
            if len(parts) == 4 and parts[:2] == ["api", "runs"] and parts[3] == "stop":
                self._send_json(self.manager.stop_run(parts[2]))
                return
            self._send_json({"error": "未找到"}, HTTPStatus.NOT_FOUND)
        except Exception as exc:
            self._send_json({"error": str(exc)}, HTTPStatus.BAD_REQUEST)

    def _stream_run(self, run_id: str) -> None:
        """处理 scripts/long_run_dashboard.py 中的 stream run 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "keep-alive")
        self.end_headers()
        for _ in range(3600):
            status = self.manager.run_status(run_id)
            payload = json.dumps(status, ensure_ascii=False)
            try:
                self.wfile.write(f"data: {payload}\n\n".encode("utf-8"))
                self.wfile.flush()
            except OSError:
                return
            if not status.get("active"):
                time.sleep(1)
                status = self.manager.run_status(run_id)
                payload = json.dumps(status, ensure_ascii=False)
                try:
                    self.wfile.write(f"data: {payload}\n\n".encode("utf-8"))
                    self.wfile.flush()
                except OSError:
                    pass
                return
            time.sleep(1)

    def _send_artifact(self, run_id: str, name: str) -> None:
        """处理 scripts/long_run_dashboard.py 中的 send artifact 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        allowed = {
            "summary_report.txt",
            "summary.json",
            "run_status.jsonl",
            "events.jsonl",
            "serial.log",
            "conversation.jsonl",
            "long_conversation.jsonl",
        }
        if name not in allowed:
            self._send_json({"error": "不允许访问该文件"}, HTTPStatus.BAD_REQUEST)
            return
        path = self.manager.output_root / run_id / name
        if not path.exists() or not path.is_file():
            self._send_json({"error": "文件不存在"}, HTTPStatus.NOT_FOUND)
            return
        content_type = mimetypes.guess_type(str(path))[0] or "text/plain"
        data = path.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", content_type + ("; charset=utf-8" if content_type.startswith("text/") else ""))
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


class QuietThreadingHTTPServer(ThreadingHTTPServer):
    def handle_error(self, request: Any, client_address: Any) -> None:
        """处理 scripts/long_run_dashboard.py 中的 handle error 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        exc_type, exc, _tb = sys.exc_info()
        if exc_type in {ConnectionAbortedError, ConnectionResetError, BrokenPipeError}:
            return
        if isinstance(exc, OSError) and getattr(exc, "winerror", None) in {10053, 10054}:
            return
        super().handle_error(request, client_address)


def build_parser() -> argparse.ArgumentParser:
    """处理 scripts/long_run_dashboard.py 中的 build parser 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    parser = argparse.ArgumentParser(description="C-Claw 长测 Web 控制台")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--output-root", default=str(DEFAULT_OUTPUT_ROOT))
    return parser


def main(argv: list[str] | None = None) -> int:
    """处理 scripts/long_run_dashboard.py 中的 main 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    args = build_parser().parse_args(argv)
    manager = RunManager(Path(args.output_root))
    DashboardHandler.manager = manager
    server = QuietThreadingHTTPServer((args.host, args.port), DashboardHandler)
    print(f"C-Claw 长测控制台: http://{args.host}:{args.port}/")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        return 0
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
