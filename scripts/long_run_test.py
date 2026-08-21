#!/usr/bin/env python3
"""Long-running serial and Web Console test tool for C-Claw.

Defaults target the current bench setup:
  serial: COM6 at 115200 baud
  web:    http://cclaw.local/
"""

from __future__ import annotations

import argparse
import http.client
import json
import os
import queue
import random
import re
import signal
import socket
import sys
import threading
import time
import traceback
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable
from urllib import error, parse, request


DEFAULT_HOST = "http://cclaw.local/"
DEFAULT_SERIAL_PORT = "COM6"
DEFAULT_BAUDRATE = 115200
DEFAULT_OUTPUT_DIR = os.path.join("outputs", "long_run_cclaw")
ALL_TEST_ITEMS = {
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
}
DEFAULT_TEST_ITEMS = [
    "status",
    "events",
    "profile",
    "ota_status",
    "config_audit",
    "network_commands",
    "serial_commands",
    "short_conversation",
    "long_conversation",
]


def utc_now() -> str:
    """处理 scripts/long_run_test.py 中的 utc now 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")


def parse_duration(text: str | None, default_seconds: float | None = None) -> float | None:
    """处理 scripts/long_run_test.py 中的 parse duration 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    if text is None or text == "":
        return default_seconds
    raw = text.strip().lower()
    if raw in {"0", "none", "forever", "infinite", "inf"}:
        return None
    units = {"s": 1, "m": 60, "h": 3600, "d": 86400}
    if raw[-1:] in units:
        return float(raw[:-1]) * units[raw[-1]]
    return float(raw)


def format_duration(seconds: float | int | None) -> str:
    """处理 scripts/long_run_test.py 中的 format duration 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    if seconds is None:
        return "forever"
    total = max(0, int(seconds))
    days, rem = divmod(total, 86400)
    hours, rem = divmod(rem, 3600)
    minutes, secs = divmod(rem, 60)
    if days:
        return f"{days}d{hours:02d}h{minutes:02d}m{secs:02d}s"
    if hours:
        return f"{hours}h{minutes:02d}m{secs:02d}s"
    if minutes:
        return f"{minutes}m{secs:02d}s"
    return f"{secs}s"


def safe_int(value: Any, default: int = 0) -> int:
    """处理 scripts/long_run_test.py 中的 safe int 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def event_seq(event: dict[str, Any]) -> int:
    """处理 scripts/long_run_test.py 中的 event seq 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    return safe_int(event.get("seq"), 0)


def compact_text(text: str, limit: int = 360) -> str:
    """处理 scripts/long_run_test.py 中的 compact text 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    one_line = " ".join((text or "").split())
    if len(one_line) <= limit:
        return one_line
    return one_line[: limit - 3] + "..."


def contains_all(text: str, tokens: Iterable[str]) -> bool:
    """处理 scripts/long_run_test.py 中的 contains all 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    lowered = (text or "").lower()
    return all(token.lower() in lowered for token in tokens)


def percentile(values: list[float], pct: float) -> float | None:
    """处理 scripts/long_run_test.py 中的 percentile 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    if not values:
        return None
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    rank = (len(ordered) - 1) * pct / 100.0
    lower = int(rank)
    upper = min(lower + 1, len(ordered) - 1)
    weight = rank - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def latency_summary(values: list[float]) -> dict[str, float | int] | None:
    """处理 scripts/long_run_test.py 中的 latency summary 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    if not values:
        return None
    return {
        "count": len(values),
        "min": round(min(values), 2),
        "p50": round(percentile(values, 50) or 0.0, 2),
        "p95": round(percentile(values, 95) or 0.0, 2),
        "max": round(max(values), 2),
    }


def format_latency_summary(values: list[float]) -> str:
    """处理 scripts/long_run_test.py 中的 format latency summary 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    stats = latency_summary(values)
    if not stats:
        return "n/a"
    return (
        f"n={stats['count']} min={stats['min']} ms "
        f"p50={stats['p50']} ms p95={stats['p95']} ms max={stats['max']} ms"
    )


def classify_http_error(message: str) -> str:
    """处理 scripts/long_run_test.py 中的 classify http error 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    lower = (message or "").lower()
    if "getaddrinfo" in lower or "name or service" in lower or "dns" in lower:
        return "dns_or_mdns"
    if "timed out" in lower or "timeout" in lower:
        return "timeout"
    if "connection reset" in lower or "connection aborted" in lower or "forcibly closed" in lower:
        return "connection_reset"
    if "connection refused" in lower:
        return "connection_refused"
    if "invalid json" in lower or "jsondecodeerror" in lower:
        return "invalid_json"
    if "failed http" in lower or "http " in lower:
        return "http_status"
    return "other"


def classify_conversation_failure(ok: bool, timing: dict[str, Any], context_ok: bool | None = None) -> str:
    """处理 scripts/long_run_test.py 中的 classify conversation failure 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    runtime_error = timing.get("last_runtime_error")
    if runtime_error:
        lower_error = str(runtime_error).lower()
        transport_markers = (
            "connect_timeout",
            "header_timeout",
            "first_body_timeout",
            "stream_idle_timeout",
            "esp_err_http_eagain",
            "eagain",
            "incomplete_data",
            "stream_incomplete",
            "stream_ended_without_done",
        )
        if any(marker in lower_error for marker in transport_markers):
            has_partial_output = (
                safe_int(timing.get("assistant_event_count"), 0) > 0 or
                safe_int(timing.get("first_output_ms"), -1) >= 0 or
                safe_int(timing.get("first_thinking_ms"), -1) >= 0 or
                safe_int(timing.get("first_token_ms"), -1) >= 0
            )
            return "partial_stream_failure" if has_partial_output else "transport_failure"
        return "runtime_error"
    if timing.get("error_event_count", 0):
        return "error_event"
    if not ok:
        if safe_int(timing.get("assistant_event_count"), 0) == 0:
            return "no_assistant_response"
        return "completion_missing"
    if context_ok is False:
        return "context_check_failed"
    return "unknown"


def apply_runtime_error_result(summary: "Summary", ok: bool, timing: dict[str, Any]) -> bool:
    """处理 scripts/long_run_test.py 中的 apply runtime error result 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    runtime_error = timing.get("last_runtime_error")
    if runtime_error:
        summary.last_error = str(runtime_error)
        return False
    if safe_int(timing.get("error_event_count"), 0) > 0:
        return False
    return ok


def make_run_id() -> str:
    """处理 scripts/long_run_test.py 中的 make run id 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    return datetime.now().strftime("run_%Y%m%d-%H%M%S-%f")[:-3]


def resolve_output_dir(base_dir: Path, append: bool, run_id: str) -> Path:
    """处理 scripts/long_run_test.py 中的 resolve output dir 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    if append:
        return base_dir
    return base_dir / run_id


def parse_test_items(text: str | None) -> set[str]:
    """处理 scripts/long_run_test.py 中的 parse test items 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    if text is None or text.strip() == "":
        return set(DEFAULT_TEST_ITEMS)
    items = {item.strip() for item in text.split(",") if item.strip()}
    unknown = sorted(items - ALL_TEST_ITEMS)
    if unknown:
        raise ValueError(f"unknown test item(s): {', '.join(unknown)}")
    return items


def has_item(args: argparse.Namespace, item: str) -> bool:
    """处理 scripts/long_run_test.py 中的 has item 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    return item in getattr(args, "test_items_set", set(DEFAULT_TEST_ITEMS))


def should_stop_for_file(args: argparse.Namespace) -> bool:
    """处理 scripts/long_run_test.py 中的 should stop for file 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    stop_file = getattr(args, "stop_file", None)
    return bool(stop_file) and Path(stop_file).exists()


class JsonlWriter:
    def __init__(self, path: Path, append: bool = False):
        """处理 scripts/long_run_test.py 中的 init 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        self.path = path
        self._lock = threading.Lock()
        self._file = path.open("a" if append else "w", encoding="utf-8", buffering=1)

    def write(self, row: dict[str, Any]) -> None:
        """处理 scripts/long_run_test.py 中的 write 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        with self._lock:
            self._file.write(json.dumps(row, ensure_ascii=False, sort_keys=True) + "\n")

    def close(self) -> None:
        """处理 scripts/long_run_test.py 中的 close 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        with self._lock:
            self._file.close()


class HttpClient:
    def __init__(self, host: str, timeout: float, persistent: bool = True):
        """处理 scripts/long_run_test.py 中的 init 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        base = host.strip()
        if not base.startswith(("http://", "https://")):
            base = "http://" + base
        self.base = base.rstrip("/")
        self.timeout = timeout
        self.persistent = persistent
        self.parsed = parse.urlsplit(self.base)
        self.scheme = self.parsed.scheme or "http"
        self.hostname = self.parsed.hostname or self.parsed.netloc
        self.port = self.parsed.port or (443 if self.scheme == "https" else 80)
        self._conn: http.client.HTTPConnection | http.client.HTTPSConnection | None = None
        self.connection_rebuilds = 0
        self.last_timing: dict[str, Any] = {}

    def _url(self, path: str, query: dict[str, Any] | None = None) -> str:
        """处理 scripts/long_run_test.py 中的 url 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        url = self.base + "/" + path.lstrip("/")
        if query:
            url += "?" + parse.urlencode(query)
        return url

    def _path(self, path: str, query: dict[str, Any] | None = None) -> str:
        """处理 scripts/long_run_test.py 中的 path 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        target = "/" + path.lstrip("/")
        if query:
            target += "?" + parse.urlencode(query)
        return target

    def close(self) -> None:
        """处理 scripts/long_run_test.py 中的 close 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        if self._conn is not None:
            try:
                self._conn.close()
            except Exception:
                pass
            self._conn = None

    def _measure_dns_ms(self) -> float | None:
        """处理 scripts/long_run_test.py 中的 measure dns ms 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        if not self.hostname:
            return None
        start = time.monotonic()
        socket.getaddrinfo(self.hostname, self.port, type=socket.SOCK_STREAM)
        return (time.monotonic() - start) * 1000.0

    def _ensure_conn(self) -> tuple[http.client.HTTPConnection | http.client.HTTPSConnection, bool, float | None]:
        """处理 scripts/long_run_test.py 中的 ensure conn 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        reused = self._conn is not None
        dns_ms: float | None = None
        if self._conn is None:
            dns_ms = self._measure_dns_ms()
            cls = http.client.HTTPSConnection if self.scheme == "https" else http.client.HTTPConnection
            self._conn = cls(self.hostname, self.port, timeout=self.timeout)
            self.connection_rebuilds += 1
        return self._conn, reused, dns_ms

    def _request_text_persistent(
        self,
        method: str,
        path: str,
        body: str | bytes | None = None,
        query: dict[str, Any] | None = None,
    ) -> tuple[int, str, float]:
        """处理 scripts/long_run_test.py 中的 request text persistent 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        data = None
        headers = {"User-Agent": "cclaw-long-run-test/1.0", "Connection": "keep-alive"}
        if body is not None:
            data = body.encode("utf-8") if isinstance(body, str) else body
            headers["Content-Type"] = "text/plain; charset=utf-8"
        start = time.monotonic()
        dns_ms: float | None = None
        reused = False
        request_ms: float | None = None
        read_ms: float | None = None
        try:
            conn, reused, dns_ms = self._ensure_conn()
            request_started = time.monotonic()
            conn.request(method, self._path(path, query), body=data, headers=headers)
            resp = conn.getresponse()
            request_ms = (time.monotonic() - request_started) * 1000.0
            read_started = time.monotonic()
            text = resp.read().decode("utf-8", errors="replace")
            read_ms = (time.monotonic() - read_started) * 1000.0
            return resp.status, text, time.monotonic() - start
        except Exception:
            self.close()
            raise
        finally:
            self.last_timing = {
                "persistent": True,
                "connection_reused": reused,
                "connection_rebuilds": self.connection_rebuilds,
                "dns_ms": None if dns_ms is None else round(dns_ms, 3),
                "request_ms": None if request_ms is None else round(request_ms, 3),
                "read_ms": None if read_ms is None else round(read_ms, 3),
            }

    def _request_text_urllib(
        self,
        method: str,
        path: str,
        body: str | bytes | None = None,
        query: dict[str, Any] | None = None,
    ) -> tuple[int, str, float]:
        """处理 scripts/long_run_test.py 中的 request text urllib 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        data = None
        headers = {"User-Agent": "cclaw-long-run-test/1.0"}
        if body is not None:
            data = body.encode("utf-8") if isinstance(body, str) else body
            headers["Content-Type"] = "text/plain; charset=utf-8"
        req = request.Request(self._url(path, query), data=data, headers=headers, method=method)
        start = time.monotonic()
        self.last_timing = {"persistent": False}
        try:
            with request.urlopen(req, timeout=self.timeout) as resp:
                read_started = time.monotonic()
                text = resp.read().decode("utf-8", errors="replace")
                self.last_timing["read_ms"] = round((time.monotonic() - read_started) * 1000.0, 3)
                return resp.getcode(), text, time.monotonic() - start
        except error.HTTPError as exc:
            read_started = time.monotonic()
            text = exc.read().decode("utf-8", errors="replace")
            self.last_timing["read_ms"] = round((time.monotonic() - read_started) * 1000.0, 3)
            return exc.code, text, time.monotonic() - start

    def request_text(
        self,
        method: str,
        path: str,
        body: str | bytes | None = None,
        query: dict[str, Any] | None = None,
    ) -> tuple[int, str, float]:
        """处理 scripts/long_run_test.py 中的 request text 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        if self.persistent and self.scheme in {"http", "https"}:
            return self._request_text_persistent(method, path, body, query)
        return self._request_text_urllib(method, path, body, query)

    def get_json(self, path: str, query: dict[str, Any] | None = None) -> tuple[dict[str, Any], float]:
        """处理 scripts/long_run_test.py 中的 get json 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        code, text, elapsed = self.request_text("GET", path, query=query)
        if code < 200 or code >= 300:
            raise RuntimeError(f"GET {path} failed HTTP {code}: {compact_text(text)}")
        try:
            value = json.loads(text)
        except json.JSONDecodeError as exc:
            raise RuntimeError(f"GET {path} returned invalid JSON: {exc}") from exc
        if not isinstance(value, dict):
            raise RuntimeError(f"GET {path} returned non-object JSON")
        return value, elapsed

    def post_message(self, text: str) -> float:
        """处理 scripts/long_run_test.py 中的 post message 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        code, body, elapsed = self.request_text("POST", "/api/messages", body=text)
        if code < 200 or code >= 300:
            raise RuntimeError(f"POST /api/messages failed HTTP {code}: {compact_text(body)}")
        return elapsed

    def ota_status(self) -> tuple[dict[str, Any], float]:
        """处理 scripts/long_run_test.py 中的 status 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        return self.get_json("/api/ota/status")

    def ota_prepare_probe(self, name: str, size: int) -> tuple[int, str, float]:
        """处理 scripts/long_run_test.py 中的 prepare probe 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        return self.request_text(
            "POST",
            "/api/ota/prepare",
            query={"name": name, "size": str(size), "time": utc_now()},
        )

    def get_config_text(self) -> tuple[str, float]:
        """处理 scripts/long_run_test.py 中的 get config text 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        code, text, elapsed = self.request_text("GET", "/api/config")
        if code < 200 or code >= 300:
            raise RuntimeError(f"GET /api/config failed HTTP {code}: {compact_text(text)}")
        return text, elapsed


class SerialWorker:
    def __init__(self, port: str, baudrate: int, log_path: Path, enabled: bool = True):
        """处理 scripts/long_run_test.py 中的 init 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        self.port = port
        self.baudrate = baudrate
        self.log_path = log_path
        self.enabled = enabled
        self.lines: queue.Queue[str] = queue.Queue()
        self.errors: queue.Queue[str] = queue.Queue()
        self.stop_event = threading.Event()
        self._thread: threading.Thread | None = None
        self._serial = None
        self._log = None
        self._close_lock = threading.Lock()
        self._echo_lock = threading.Lock()
        self._pending_echoes: list[str] = []
        self._closed = False
        self.available = False

    def start(self) -> None:
        """处理 scripts/long_run_test.py 中的 start 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        if not self.enabled or self.stop_event.is_set():
            return
        try:
            import serial  # type: ignore
        except ImportError:
            self.errors.put("pyserial is not installed; install scripts/requirements-long-run.txt")
            return
        try:
            self._serial = serial.Serial(self.port, self.baudrate, timeout=0.2, write_timeout=2)
            self._log = self.log_path.open("a", encoding="utf-8", buffering=1)
        except Exception as exc:  # pragma: no cover - hardware dependent
            self.errors.put(f"open serial {self.port} failed: {exc}")
            return
        self.available = True
        self._thread = threading.Thread(target=self._read_loop, name="serial-reader", daemon=True)
        self._thread.start()

    def _read_loop(self) -> None:
        """处理 scripts/long_run_test.py 中的 read loop 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        assert self._serial is not None
        assert self._log is not None
        buffer = bytearray()
        while not self.stop_event.is_set():
            try:
                chunk = self._serial.read(256)
                if not chunk:
                    continue
                for byte in chunk:
                    if byte in (10, 13):
                        if buffer:
                            line = buffer.decode("utf-8", errors="replace")
                            self._record_line(line)
                            buffer.clear()
                    else:
                        buffer.append(byte)
                        if len(buffer) >= 2048:
                            line = buffer.decode("utf-8", errors="replace")
                            self._record_line(line)
                            buffer.clear()
            except Exception as exc:  # pragma: no cover - hardware dependent
                self.errors.put(f"serial read failed: {exc}")
                time.sleep(1)

    def _record_line(self, line: str) -> None:
        """处理 scripts/long_run_test.py 中的 record line 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        if self._consume_command_echo(line):
            return
        assert self._log is not None
        self._log.write(line + "\n")
        self.lines.put(line)

    def _consume_command_echo(self, line: str) -> bool:
        """处理 scripts/long_run_test.py 中的 consume command echo 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        candidate = line.strip()
        if candidate.startswith("cclaw>"):
            candidate = candidate[len("cclaw>"):].strip()
        if not candidate:
            return False
        with self._echo_lock:
            for index, expected in enumerate(self._pending_echoes):
                if candidate == expected:
                    del self._pending_echoes[index]
                    return True
        return False

    def write_line(self, text: str) -> bool:
        """处理 scripts/long_run_test.py 中的 write line 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        if not self.available or self._serial is None:
            return False
        try:
            with self._echo_lock:
                self._pending_echoes.append(text.strip())
                if len(self._pending_echoes) > 8:
                    del self._pending_echoes[:-8]
            self._serial.write((text + "\r\n").encode("utf-8"))
            self._serial.flush()
            return True
        except Exception as exc:  # pragma: no cover - hardware dependent
            with self._echo_lock:
                stripped = text.strip()
                for index in range(len(self._pending_echoes) - 1, -1, -1):
                    if self._pending_echoes[index] == stripped:
                        del self._pending_echoes[index]
                        break
            self.errors.put(f"serial write failed: {exc}")
            return False

    def request_close(self) -> None:
        """处理 scripts/long_run_test.py 中的 request close 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        self.stop_event.set()
        self.available = False
        with self._close_lock:
            if self._closed:
                return
            if self._serial is not None:
                try:
                    self._serial.close()
                except Exception:
                    pass
                self._serial = None

    def recent_lines(self, max_lines: int = 80) -> list[str]:
        """处理 scripts/long_run_test.py 中的 recent lines 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        out: list[str] = []
        while len(out) < max_lines:
            try:
                out.append(self.lines.get_nowait())
            except queue.Empty:
                break
        return out

    def recent_errors(self) -> list[str]:
        """处理 scripts/long_run_test.py 中的 recent errors 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        out: list[str] = []
        while True:
            try:
                out.append(self.errors.get_nowait())
            except queue.Empty:
                return out

    def close(self) -> None:
        """处理 scripts/long_run_test.py 中的 close 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        self.request_close()
        if self._thread:
            self._thread.join(timeout=2)
        with self._close_lock:
            if self._closed:
                return
            if self._log is not None:
                self._log.close()
                self._log = None
            self._closed = True


@dataclass
class Summary:
    run_id: str | None = None
    run_label: str | None = None
    output_dir: str | None = None
    started_at: str = field(default_factory=utc_now)
    finished_at: str | None = None
    requested_duration_seconds: float | None = None
    elapsed_seconds: float = 0.0
    stop_reason: str | None = None
    signal: str | None = None
    incomplete_run: bool = False
    loops: int = 0
    http_ok: int = 0
    http_errors: int = 0
    http_latency_ms: dict[str, list[float]] = field(default_factory=dict)
    http_dns_latency_ms: dict[str, list[float]] = field(default_factory=dict)
    http_request_latency_ms: dict[str, list[float]] = field(default_factory=dict)
    http_read_latency_ms: dict[str, list[float]] = field(default_factory=dict)
    http_connection_rebuilds: int = 0
    http_persistent_enabled: bool = True
    http_error_categories: dict[str, int] = field(default_factory=dict)
    serial_writes: int = 0
    serial_write_failures: int = 0
    serial_errors: int = 0
    events_seen: int = 0
    event_duplicate_seq: int = 0
    event_backward_seq: int = 0
    event_seq_gap_count: int = 0
    event_seq_gap_lost: int = 0
    event_latest_seq_reported: int = 0
    max_event_seq: int = 0
    max_events_dropped: int = 0
    max_events_ring_overwrites: int = 0
    max_events_sink_dropped: int = 0
    min_events_oldest_seq: int | None = None
    max_events_capacity: int = 0
    min_internal_heap: int | None = None
    min_psram_heap: int | None = None
    reboots_observed: int = 0
    short_conversations: int = 0
    short_conversation_failures: int = 0
    long_conversations: int = 0
    long_conversation_turns: int = 0
    long_conversation_failures: int = 0
    context_check_failures: int = 0
    skipped_commands: int = 0
    idle_waits: int = 0
    idle_timeouts: int = 0
    conversation_timeouts: int = 0
    conversation_timing_samples: int = 0
    first_output_total_ms: float = 0.0
    first_thinking_total_ms: float = 0.0
    first_token_total_ms: float = 0.0
    total_elapsed_total_ms: float = 0.0
    max_total_elapsed_ms: float = 0.0
    first_output_samples_ms: list[float] = field(default_factory=list)
    first_thinking_samples_ms: list[float] = field(default_factory=list)
    first_token_samples_ms: list[float] = field(default_factory=list)
    total_elapsed_samples_ms: list[float] = field(default_factory=list)
    conversation_failure_reasons: dict[str, int] = field(default_factory=dict)
    last_error: str | None = None
    profile_events: int = 0
    profile_parse_errors: int = 0
    profile_partial_events: int = 0
    profile_min_internal_heap: int | None = None
    profile_min_psram_heap: int | None = None
    profile_min_psram_largest: int | None = None
    profile_min_stack_words: int | None = None
    profile_phase_counts: dict[str, int] = field(default_factory=dict)
    profile_phase_total_us: dict[str, float] = field(default_factory=dict)
    profile_phase_max_us: dict[str, float] = field(default_factory=dict)
    profile_cpu_available_samples: int = 0
    profile_cpu_unavailable_samples: int = 0
    profile_cpu_task_samples: dict[str, int] = field(default_factory=dict)
    profile_cpu_task_total_pct: dict[str, float] = field(default_factory=dict)
    profile_cpu_task_max_pct: dict[str, float] = field(default_factory=dict)
    profile_cpu_task_last_pct: dict[str, float] = field(default_factory=dict)
    profile_cpu_total_samples: int = 0
    profile_cpu_total_sum_pct: float = 0.0
    profile_cpu_total_max_pct: float = 0.0
    profile_cpu_total_last_pct: float | None = None
    profile_cpu_core_samples: dict[str, int] = field(default_factory=dict)
    profile_cpu_core_total_pct: dict[str, float] = field(default_factory=dict)
    profile_cpu_core_max_pct: dict[str, float] = field(default_factory=dict)
    profile_cpu_core_last_pct: dict[str, float] = field(default_factory=dict)
    profile_expected_level: str | None = None
    profile_expected_interval_ms: int | None = None
    profile_mismatch_count: int = 0
    profile_mismatches: list[dict[str, Any]] = field(default_factory=list)
    ota_status_samples: int = 0
    ota_last_state: str | None = None
    ota_last_manager_state: str | None = None
    ota_last_failure_code: str | None = None
    ota_failure_counts: dict[str, int] = field(default_factory=dict)
    ota_rollback_pending_seen: bool = False
    ota_min_psram_largest: int | None = None
    ota_min_internal_largest: int | None = None
    ota_max_expected_size: int = 0
    fault_injection_results: list[dict[str, Any]] = field(default_factory=list)
    config_audit_risks: list[str] = field(default_factory=list)
    config_audit_checked: bool = False
    llm_http_attempts: int = 0
    llm_http_attempt_ok: int = 0
    llm_http_attempt_error: int = 0
    llm_http_attempt_stage_counts: dict[str, int] = field(default_factory=dict)
    llm_http_attempt_pool_counts: dict[str, int] = field(default_factory=dict)
    llm_http_attempt_discard_counts: dict[str, int] = field(default_factory=dict)
    llm_http_attempt_retry_allowed: int = 0
    llm_http_attempt_retry_performed: int = 0
    llm_http_attempt_partial_stream: int = 0
    llm_http_attempt_saw_done: int = 0
    llm_http_attempt_max_reuse_reached: int = 0
    llm_http_attempt_ids: set[int] = field(default_factory=set)
    llm_http_attempt_error_ids: set[int] = field(default_factory=set)
    serial_http_attempt_failed: int = 0
    serial_http_attempt_stage_counts: dict[str, int] = field(default_factory=dict)
    serial_http_attempt_failed_ids: set[int] = field(default_factory=set)
    serial_http_attempt_failures: list[dict[str, Any]] = field(default_factory=list)
    llm_attempt_consistency_mismatch: bool = False
    llm_phase_by_correlation: dict[str, dict[str, Any]] = field(default_factory=dict)
    long_failure_diagnostics: list[dict[str, Any]] = field(default_factory=list)
    _seen_event_seqs: set[int] = field(default_factory=set, repr=False)
    _last_event_seq_observed: int | None = field(default=None, repr=False)

    def observe_status(self, status: dict[str, Any], previous_uptime_ms: int | None) -> int | None:
        """处理 scripts/long_run_test.py 中的 observe status 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        uptime = safe_int(status.get("uptime_ms"), -1)
        if previous_uptime_ms is not None and uptime >= 0 and uptime + 5000 < previous_uptime_ms:
            self.reboots_observed += 1
        heap = status.get("heap") if isinstance(status.get("heap"), dict) else {}
        internal = safe_int(heap.get("internal_free"), -1)
        psram = safe_int(heap.get("psram_free"), -1)
        if internal >= 0:
            self.min_internal_heap = internal if self.min_internal_heap is None else min(self.min_internal_heap, internal)
        if psram >= 0:
            self.min_psram_heap = psram if self.min_psram_heap is None else min(self.min_psram_heap, psram)
        events = status.get("events") if isinstance(status.get("events"), dict) else {}
        self.max_events_dropped = max(self.max_events_dropped, safe_int(events.get("dropped"), 0))
        self.max_events_ring_overwrites = max(self.max_events_ring_overwrites, safe_int(events.get("ring_overwrites"), 0))
        self.max_events_sink_dropped = max(self.max_events_sink_dropped, safe_int(events.get("sink_dropped"), 0))
        oldest_seq = safe_int(events.get("oldest_seq"), -1)
        if oldest_seq >= 0:
            self.min_events_oldest_seq = oldest_seq if self.min_events_oldest_seq is None else min(self.min_events_oldest_seq, oldest_seq)
        self.max_events_capacity = max(self.max_events_capacity, safe_int(events.get("capacity"), 0))

    def observe_ota_status(self, status: dict[str, Any]) -> None:
        """处理 scripts/long_run_test.py 中的 observe ota status 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        self.ota_status_samples += 1
        state = status.get("state")
        manager_state = status.get("manager_state")
        failure_code = status.get("failure_code")
        if isinstance(state, str):
            self.ota_last_state = state
        if isinstance(manager_state, str):
            self.ota_last_manager_state = manager_state
        if isinstance(failure_code, str):
            self.ota_last_failure_code = failure_code
            if failure_code and failure_code != "none":
                self.ota_failure_counts[failure_code] = self.ota_failure_counts.get(failure_code, 0) + 1
        self.ota_rollback_pending_seen = self.ota_rollback_pending_seen or bool(status.get("rollback_pending"))
        resources = status.get("resource_snapshot") if isinstance(status.get("resource_snapshot"), dict) else {}
        psram_largest = safe_int(resources.get("psram_largest"), -1)
        internal_largest = safe_int(resources.get("internal_largest"), -1)
        expected_size = safe_int(resources.get("expected_size"), -1)
        if psram_largest >= 0:
            self.ota_min_psram_largest = psram_largest if self.ota_min_psram_largest is None else min(self.ota_min_psram_largest, psram_largest)
        if internal_largest >= 0:
            self.ota_min_internal_largest = internal_largest if self.ota_min_internal_largest is None else min(self.ota_min_internal_largest, internal_largest)
        if expected_size >= 0:
            self.ota_max_expected_size = max(self.ota_max_expected_size, expected_size)

    def observe_fault_result(self, row: dict[str, Any]) -> None:
        """处理 scripts/long_run_test.py 中的 observe fault result 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        if len(self.fault_injection_results) < 50:
            self.fault_injection_results.append(row)

    def observe_config_audit(self, risks: list[str]) -> None:
        """处理 scripts/long_run_test.py 中的 observe config audit 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        self.config_audit_checked = True
        self.config_audit_risks = sorted(set(risks))

    def observe_http_latency(self, endpoint: str, elapsed_seconds: float) -> None:
        """处理 scripts/long_run_test.py 中的 observe http latency 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        self.http_latency_ms.setdefault(endpoint, []).append(round(elapsed_seconds * 1000.0, 3))

    def observe_http_client_timing(self, endpoint: str, timing: dict[str, Any]) -> None:
        """处理 scripts/long_run_test.py 中的 observe http client timing 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        if not timing:
            return
        dns_ms = timing.get("dns_ms")
        if isinstance(dns_ms, (int, float)):
            self.http_dns_latency_ms.setdefault(endpoint, []).append(float(dns_ms))
        request_ms = timing.get("request_ms")
        if isinstance(request_ms, (int, float)):
            self.http_request_latency_ms.setdefault(endpoint, []).append(float(request_ms))
        read_ms = timing.get("read_ms")
        if isinstance(read_ms, (int, float)):
            self.http_read_latency_ms.setdefault(endpoint, []).append(float(read_ms))
        self.http_connection_rebuilds = max(
            self.http_connection_rebuilds,
            safe_int(timing.get("connection_rebuilds"), self.http_connection_rebuilds),
        )

    def observe_http_error(self, message: str) -> None:
        """处理 scripts/long_run_test.py 中的 observe http error 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        category = classify_http_error(message)
        self.http_error_categories[category] = self.http_error_categories.get(category, 0) + 1

    def observe_profile_config(self, status: dict[str, Any]) -> None:
        """处理 scripts/long_run_test.py 中的 observe profile config 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        profile = status.get("profile") if isinstance(status.get("profile"), dict) else {}
        if not profile:
            return
        mismatches: dict[str, Any] = {}
        actual_level = profile.get("level")
        actual_interval = profile.get("interval_ms")
        if self.profile_expected_level and actual_level != self.profile_expected_level:
            mismatches["level"] = {"expected": self.profile_expected_level, "actual": actual_level}
        if self.profile_expected_interval_ms is not None and actual_interval != self.profile_expected_interval_ms:
            mismatches["interval_ms"] = {
                "expected": self.profile_expected_interval_ms,
                "actual": actual_interval,
            }
        if mismatches:
            self.profile_mismatch_count += 1
            if len(self.profile_mismatches) < 20:
                self.profile_mismatches.append({"ts": utc_now(), **mismatches})

    def observe_serial_line(self, line: str) -> None:
        """处理 scripts/long_run_test.py 中的 observe serial line 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        if "HTTP attempt failed" not in (line or ""):
            return
        self.serial_http_attempt_failed += 1
        match = re.search(r"\bstage=([^\s]+)", line)
        stage = match.group(1) if match else "unknown"
        self.serial_http_attempt_stage_counts[stage] = (
            self.serial_http_attempt_stage_counts.get(stage, 0) + 1
        )
        attempt_match = re.search(r"\battempt=(\d+)", line)
        attempt_id = safe_int(attempt_match.group(1), -1) if attempt_match else -1
        err_match = re.search(r"\berr=([^\s]+)", line)
        status_match = re.search(r"\bstatus=(-?\d+)", line)
        detail = {
            "attempt_id": attempt_id if attempt_id >= 0 else None,
            "stage": stage,
            "err": err_match.group(1) if err_match else None,
            "status": safe_int(status_match.group(1), -1) if status_match else None,
            "line": compact_text(line, 300),
        }
        if attempt_id >= 0:
            self.serial_http_attempt_failed_ids.add(attempt_id)
        if len(self.serial_http_attempt_failures) < 50:
            self.serial_http_attempt_failures.append(detail)

    def observe_events_payload(self, payload: dict[str, Any]) -> None:
        """处理 scripts/long_run_test.py 中的 observe events payload 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        self.event_latest_seq_reported = max(self.event_latest_seq_reported, safe_int(payload.get("latest_seq"), 0))
        self.max_events_dropped = max(self.max_events_dropped, safe_int(payload.get("dropped"), 0))
        self.max_events_ring_overwrites = max(self.max_events_ring_overwrites, safe_int(payload.get("ring_overwrites"), 0))
        self.max_events_sink_dropped = max(self.max_events_sink_dropped, safe_int(payload.get("sink_dropped"), 0))
        oldest_seq = safe_int(payload.get("oldest_seq"), -1)
        if oldest_seq >= 0:
            self.min_events_oldest_seq = oldest_seq if self.min_events_oldest_seq is None else min(self.min_events_oldest_seq, oldest_seq)
        self.max_events_capacity = max(self.max_events_capacity, safe_int(payload.get("capacity"), 0))

    def observe_event_seq(self, seq: int) -> None:
        """处理 scripts/long_run_test.py 中的 observe event seq 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        if seq <= 0:
            return
        self.events_seen += 1
        self.max_event_seq = max(self.max_event_seq, seq)
        if seq in self._seen_event_seqs:
            self.event_duplicate_seq += 1
            return
        self._seen_event_seqs.add(seq)
        if self._last_event_seq_observed is not None:
            if seq < self._last_event_seq_observed:
                self.event_backward_seq += 1
            elif seq > self._last_event_seq_observed + 1:
                self.event_seq_gap_count += 1
                self.event_seq_gap_lost += seq - self._last_event_seq_observed - 1
        self._last_event_seq_observed = seq

    def fail(self, message: str, http: bool = False) -> None:
        """处理 scripts/long_run_test.py 中的 fail 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        self.last_error = message
        if http:
            self.http_errors += 1
            self.observe_http_error(message)

    def observe_conversation_failure(self, reason: str) -> None:
        """处理 scripts/long_run_test.py 中的 observe conversation failure 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        key = reason or "unknown"
        self.conversation_failure_reasons[key] = self.conversation_failure_reasons.get(key, 0) + 1

    def observe_conversation_timing(self, timing: dict[str, Any]) -> None:
        """处理 scripts/long_run_test.py 中的 observe conversation timing 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        total = timing.get("total_elapsed_ms")
        if isinstance(total, (int, float)):
            value = float(total)
            self.conversation_timing_samples += 1
            self.total_elapsed_total_ms += value
            self.total_elapsed_samples_ms.append(value)
            self.max_total_elapsed_ms = max(self.max_total_elapsed_ms, value)
        first = timing.get("first_token_after_ms")
        if isinstance(first, (int, float)):
            value = float(first)
            self.first_token_total_ms += value
            self.first_token_samples_ms.append(value)
        first_thinking = timing.get("first_thinking_after_ms")
        if isinstance(first_thinking, (int, float)):
            value = float(first_thinking)
            self.first_thinking_total_ms += value
            self.first_thinking_samples_ms.append(value)
        first_output = timing.get("first_output_after_ms")
        if isinstance(first_output, (int, float)):
            value = float(first_output)
            self.first_output_total_ms += value
            self.first_output_samples_ms.append(value)

    def observe_profile_payload(self, payload: dict[str, Any]) -> None:
        """处理 scripts/long_run_test.py 中的 observe profile payload 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        self.profile_events += 1
        phase = str(payload.get("phase") or payload.get("sdk_event") or payload.get("event") or "unknown")
        elapsed = payload.get("elapsed_us")
        self.profile_phase_counts[phase] = self.profile_phase_counts.get(phase, 0) + 1
        if isinstance(elapsed, (int, float)):
            value = float(elapsed)
            self.profile_phase_total_us[phase] = self.profile_phase_total_us.get(phase, 0.0) + value
            self.profile_phase_max_us[phase] = max(self.profile_phase_max_us.get(phase, 0.0), value)

        correlation_id = str(payload.get("correlation_id") or "")
        if correlation_id:
            slot = self.llm_phase_by_correlation.setdefault(
                correlation_id,
                {"session_id": payload.get("session_id"), "step": payload.get("step"), "phases": {}},
            )
            phases = slot.setdefault("phases", {})
            phase_slot = phases.setdefault(phase, {"count": 0, "statuses": {}, "max_elapsed_us": 0})
            phase_slot["count"] += 1
            status = str(payload.get("status") or "")
            if status:
                phase_slot["statuses"][status] = phase_slot["statuses"].get(status, 0) + 1
            if isinstance(elapsed, (int, float)):
                phase_slot["max_elapsed_us"] = max(phase_slot["max_elapsed_us"], float(elapsed))

        if phase == "llm.http.attempt":
            self.llm_http_attempts += 1
            attrs = payload.get("attributes") if isinstance(payload.get("attributes"), dict) else {}
            stage = str(attrs.get("stage") or "unknown")
            attempt_id = safe_int(attrs.get("attempt_id"), -1)
            pool = str(attrs.get("connection_pool") or "unknown")
            discard_reason = str(attrs.get("connection_discard_reason") or "")
            if attempt_id >= 0:
                self.llm_http_attempt_ids.add(attempt_id)
            self.llm_http_attempt_stage_counts[stage] = self.llm_http_attempt_stage_counts.get(stage, 0) + 1
            self.llm_http_attempt_pool_counts[pool] = self.llm_http_attempt_pool_counts.get(pool, 0) + 1
            if discard_reason:
                self.llm_http_attempt_discard_counts[discard_reason] = (
                    self.llm_http_attempt_discard_counts.get(discard_reason, 0) + 1
                )
            if attrs.get("retry_allowed") is True:
                self.llm_http_attempt_retry_allowed += 1
            if attrs.get("retry_performed") is True:
                self.llm_http_attempt_retry_performed += 1
            if attrs.get("partial_stream") is True:
                self.llm_http_attempt_partial_stream += 1
            if attrs.get("saw_done") is True:
                self.llm_http_attempt_saw_done += 1
            if attrs.get("max_reuse_reached") is True:
                self.llm_http_attempt_max_reuse_reached += 1
            status = str(payload.get("status") or "")
            if status == "ok" or stage == "ok":
                self.llm_http_attempt_ok += 1
            else:
                self.llm_http_attempt_error += 1
                if attempt_id >= 0:
                    self.llm_http_attempt_error_ids.add(attempt_id)

        heap = payload.get("heap") if isinstance(payload.get("heap"), dict) else {}
        internal = safe_int(heap.get("internal_free"), -1)
        psram = safe_int(heap.get("psram_free"), -1)
        psram_largest = safe_int(heap.get("psram_largest"), -1)
        if internal >= 0:
            self.profile_min_internal_heap = internal if self.profile_min_internal_heap is None else min(self.profile_min_internal_heap, internal)
        if psram >= 0:
            self.profile_min_psram_heap = psram if self.profile_min_psram_heap is None else min(self.profile_min_psram_heap, psram)
        if psram_largest >= 0:
            self.profile_min_psram_largest = psram_largest if self.profile_min_psram_largest is None else min(self.profile_min_psram_largest, psram_largest)

        stack = payload.get("stack") if isinstance(payload.get("stack"), dict) else {}
        for value in stack.values():
            words = safe_int(value, -1)
            if words >= 0:
                self.profile_min_stack_words = words if self.profile_min_stack_words is None else min(self.profile_min_stack_words, words)

        cpu = payload.get("cpu") if isinstance(payload.get("cpu"), dict) else {}
        if cpu:
            if cpu.get("available") is True:
                self.profile_cpu_available_samples += 1
            else:
                self.profile_cpu_unavailable_samples += 1
            tasks = cpu.get("tasks") if isinstance(cpu.get("tasks"), list) else []
            idle_pct_values: list[float] = []
            idle_pct_by_core: dict[int, float] = {}
            for task in tasks:
                if not isinstance(task, dict):
                    continue
                name = str(task.get("name") or task.get("n") or "unknown")
                pct = task.get("core_pct")
                if pct is None:
                    pct = task.get("cp")
                if pct is None:
                    pct = task.get("cpu_pct")
                if pct is None:
                    pct = task.get("p")
                if not isinstance(pct, (int, float)):
                    continue
                value = float(pct)
                self.profile_cpu_task_samples[name] = self.profile_cpu_task_samples.get(name, 0) + 1
                self.profile_cpu_task_total_pct[name] = self.profile_cpu_task_total_pct.get(name, 0.0) + value
                self.profile_cpu_task_max_pct[name] = max(self.profile_cpu_task_max_pct.get(name, 0.0), value)
                self.profile_cpu_task_last_pct[name] = value
                core = task.get("core")
                if core is None:
                    core = task.get("c")
                if name.upper().startswith("IDLE"):
                    idle_pct_values.append(value)
                    if isinstance(core, int) and core >= 0:
                        idle_pct_by_core[core] = value
                    elif name[-1:].isdigit():
                        idle_pct_by_core[int(name[-1])] = value
            core_pct = cpu.get("core_pct")
            if isinstance(core_pct, list):
                for index, pct_value in enumerate(core_pct):
                    if isinstance(pct_value, (int, float)):
                        self._observe_cpu_core_pct(str(index), float(pct_value))
            elif idle_pct_by_core:
                for core_id, idle_value in idle_pct_by_core.items():
                    self._observe_cpu_core_pct(str(core_id), 100.0 - idle_value)
            total_pct = cpu.get("total_pct")
            if not isinstance(total_pct, (int, float)) and idle_pct_values:
                cores = cpu.get("cores")
                if not isinstance(cores, (int, float)) or cores <= 0:
                    cores = max(1, len(idle_pct_values))
                total_pct = 100.0 - (sum(idle_pct_values) / float(cores))
            if isinstance(total_pct, (int, float)):
                total_value = max(0.0, min(100.0, float(total_pct)))
                self.profile_cpu_total_samples += 1
                self.profile_cpu_total_sum_pct += total_value
                self.profile_cpu_total_max_pct = max(self.profile_cpu_total_max_pct, total_value)
                self.profile_cpu_total_last_pct = total_value

    def _observe_cpu_core_pct(self, core: str, value: float) -> None:
        """处理 scripts/long_run_test.py 中的 observe cpu core pct 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        total_value = max(0.0, min(100.0, float(value)))
        self.profile_cpu_core_samples[core] = self.profile_cpu_core_samples.get(core, 0) + 1
        self.profile_cpu_core_total_pct[core] = self.profile_cpu_core_total_pct.get(core, 0.0) + total_value
        self.profile_cpu_core_max_pct[core] = max(self.profile_cpu_core_max_pct.get(core, 0.0), total_value)
        self.profile_cpu_core_last_pct[core] = total_value

    def as_dict(self) -> dict[str, Any]:
        """处理 scripts/long_run_test.py 中的 as dict 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        data = dict(self.__dict__)
        data["finished_at"] = self.finished_at
        data["http_latency_summary_ms"] = {
            key: latency_summary(values)
            for key, values in self.http_latency_ms.items()
            if latency_summary(values)
        }
        data["http_dns_latency_summary_ms"] = {
            key: latency_summary(values)
            for key, values in self.http_dns_latency_ms.items()
            if latency_summary(values)
        }
        data["http_request_latency_summary_ms"] = {
            key: latency_summary(values)
            for key, values in self.http_request_latency_ms.items()
            if latency_summary(values)
        }
        data["http_read_latency_summary_ms"] = {
            key: latency_summary(values)
            for key, values in self.http_read_latency_ms.items()
            if latency_summary(values)
        }
        data["llm_attempt_consistency_mismatch"] = (
            self.serial_http_attempt_failed != self.llm_http_attempt_error
        )
        missing_profile_failed_ids = sorted(
            attempt_id for attempt_id in self.serial_http_attempt_failed_ids
            if attempt_id not in self.llm_http_attempt_ids
        )
        data["llm_http_attempt_ids"] = sorted(self.llm_http_attempt_ids)
        data["llm_http_attempt_error_ids"] = sorted(self.llm_http_attempt_error_ids)
        data["serial_http_attempt_failed_ids"] = sorted(self.serial_http_attempt_failed_ids)
        data["llm_http_attempt_missing_profile_failed_ids"] = missing_profile_failed_ids
        data["llm_attempt_consistency_mismatch"] = (
            self.serial_http_attempt_failed != self.llm_http_attempt_error or
            bool(missing_profile_failed_ids)
        )
        data["first_output_summary_ms"] = latency_summary(self.first_output_samples_ms)
        data["first_thinking_summary_ms"] = latency_summary(self.first_thinking_samples_ms)
        data["first_token_summary_ms"] = latency_summary(self.first_token_samples_ms)
        data["total_elapsed_summary_ms"] = latency_summary(self.total_elapsed_samples_ms)
        data["profile_cpu_tasks"] = {
            name: {
                "samples": self.profile_cpu_task_samples.get(name, 0),
                "avg_pct": round(
                    self.profile_cpu_task_total_pct.get(name, 0.0) /
                    max(1, self.profile_cpu_task_samples.get(name, 0)),
                    2,
                ),
                "max_pct": round(self.profile_cpu_task_max_pct.get(name, 0.0), 2),
                "last_pct": round(self.profile_cpu_task_last_pct.get(name, 0.0), 2),
            }
            for name in sorted(self.profile_cpu_task_samples)
        }
        data["profile_cpu_total"] = {
            "samples": self.profile_cpu_total_samples,
            "avg_pct": round(self.profile_cpu_total_sum_pct / max(1, self.profile_cpu_total_samples), 2),
            "max_pct": round(self.profile_cpu_total_max_pct, 2),
            "last_pct": None if self.profile_cpu_total_last_pct is None else round(self.profile_cpu_total_last_pct, 2),
        }
        data["profile_cpu_cores"] = {
            core: {
                "samples": self.profile_cpu_core_samples.get(core, 0),
                "avg_pct": round(self.profile_cpu_core_total_pct.get(core, 0.0) / max(1, self.profile_cpu_core_samples.get(core, 0)), 2),
                "max_pct": round(self.profile_cpu_core_max_pct.get(core, 0.0), 2),
                "last_pct": round(self.profile_cpu_core_last_pct.get(core, 0.0), 2),
            }
            for core in sorted(self.profile_cpu_core_samples)
        }
        data["unique_event_seqs"] = len(self._seen_event_seqs)
        data.pop("_seen_event_seqs", None)
        data.pop("_last_event_seq_observed", None)
        return data


SHORT_PROMPTS = [
    "请简短说明当前设备运行状态，并在回答中包含 short_chat_ok。",
    "请根据你能看到的上下文，用两句话说明 C-Claw 正在做什么。",
    "请回复当前交互是否正常，并包含关键词 serial_web_test。",
]

SERIAL_COMMANDS = [
    "/help",
    "/heap",
    "/debug",
    '/lua print("long_run_ok")',
]

NETWORK_COMMANDS = [
    "/help",
    "/heap",
    '/lua print("net_long_run_ok")',
]


def is_slow_command(text: str) -> bool:
    """处理 scripts/long_run_test.py 中的 is slow command 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    stripped = (text or "").strip()
    return stripped.startswith("/lua ") or stripped.startswith("/luafile ")


def status_is_idle(flat_status: dict[str, Any] | None) -> bool:
    """处理 scripts/long_run_test.py 中的 status is idle 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    if not flat_status:
        return False
    return flat_status.get("interaction_busy") is False and safe_int(flat_status.get("interaction_pending"), 0) == 0


def status_busy_pending(flat_status: dict[str, Any] | None) -> tuple[Any, Any]:
    """处理 scripts/long_run_test.py 中的 status busy pending 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    if not flat_status:
        return "?", "?"
    return flat_status.get("interaction_busy", "?"), flat_status.get("interaction_pending", "?")


def make_long_conversation(turns: int) -> list[dict[str, Any]]:
    """处理 scripts/long_run_test.py 中的 make long conversation 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    turns = max(3, min(20, turns))
    task_id = f"LR-{random.randint(1000, 9999)}"
    device_name = "C-Claw-COM6"
    param = f"P{random.randint(20, 89)}"
    base = [
        {
            "prompt": (
                f"开始一个长对话稳定性测试。请记住：任务编号是 {task_id}，设备名是 {device_name}，"
                f"关键参数是 {param}。先确认你已记住这些信息，并用中文回答。"
            ),
            "expect": [task_id, device_name, param],
        },
        {
            "prompt": "现在请设计一个三步检查流程，依次检查网络状态、串口命令和 USB 日志。回答要引用刚才的任务编号。",
            "expect": [task_id],
        },
        {
            "prompt": f"把关键参数 {param} 用到检查流程里：假设它是采样批次号，请说明每一步如何记录它。",
            "expect": [param],
        },
        {
            "prompt": "中间插入一个上下文检查：请复述设备名、任务编号、关键参数，并说明目前流程的目标。",
            "expect": [task_id, device_name, param],
        },
        {
            "prompt": (
                "下面是一段较长约束：长测不能频繁重启设备，不能覆盖 USB 配置文件，"
                "需要保留串口原始日志、Web 事件日志、每轮状态快照，并且长对话失败也要继续后续测试。"
                "请提取这些约束，并继续沿用前面的任务编号。"
            ),
            "expect": [task_id],
        },
        {
            "prompt": "现在把之前的检查流程改成更适合 6 小时烧机的版本，要求提到关键参数和错误恢复。",
            "expect": [param],
        },
        {
            "prompt": "请假设刚刚发生了一次 Wi-Fi 短暂断连，说明测试脚本应如何记录并恢复，仍然引用任务编号。",
            "expect": [task_id],
        },
        {
            "prompt": "最终总结整个长对话测试结论，必须包含设备名、任务编号和关键参数。",
            "expect": [task_id, device_name, param],
        },
    ]
    if turns <= len(base):
        return base[:turns]
    extra: list[dict[str, Any]] = []
    for idx in range(turns - len(base)):
        extra.append(
            {
                "prompt": f"继续第 {idx + 1} 个扩展追问：结合前文说明一个额外风险和一个观测指标，引用 {task_id}。",
                "expect": [task_id],
            }
        )
    return base + extra


def make_long_conversation(turns: int) -> list[dict[str, Any]]:
    """处理 scripts/long_run_test.py 中的 make long conversation 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    turns = max(3, min(20, turns))
    task_id = f"LR-{random.randint(1000, 9999)}"
    device_name = "C-Claw-COM6"
    param = f"P{random.randint(20, 89)}"
    base = [
        {
            "prompt": (
                f"开始一个长对话稳定性测试。请记住：任务编号是 {task_id}，设备名是 {device_name}，"
                f"关键参数是 {param}。先确认你已记住这些信息，并用中文回答。"
            ),
            "expect": [task_id, device_name, param],
        },
        {
            "prompt": "现在请设计一个三步检查流程，依次检查网络状态、串口命令和 USB 日志。回答要引用刚才的任务编号。",
            "expect": [task_id],
        },
        {
            "prompt": f"把关键参数 {param} 用到检查流程里：假设它是采样批次号，请说明每一步如何记录它。",
            "expect": [param],
        },
        {
            "prompt": "中间插入一个上下文检查：请复述设备名、任务编号、关键参数，并说明目前流程的目标。",
            "expect": [task_id, device_name, param],
        },
        {
            "prompt": (
                "下面是一段较长约束：长测不能频繁重启设备，不能覆盖 USB 配置文件，"
                "需要保留串口原始日志、Web 事件日志、每轮状态快照，并且长对话失败也要继续后续测试。"
                "请提取这些约束，并继续沿用前面的任务编号。"
            ),
            "expect": [task_id],
        },
        {
            "prompt": "现在把之前的检查流程改成更适合 6 小时烧机的版本，要求提到关键参数和错误恢复。",
            "expect": [param],
        },
        {
            "prompt": "请假设刚刚发生了一次 Wi-Fi 短暂断连，说明测试脚本应如何记录并恢复，仍然引用任务编号。",
            "expect": [task_id],
        },
        {
            "prompt": "最终总结整个长对话测试结论，必须包含设备名、任务编号和关键参数。",
            "expect": [task_id, device_name, param],
        },
    ]
    if turns <= len(base):
        return base[:turns]
    extra: list[dict[str, Any]] = []
    for idx in range(turns - len(base)):
        extra.append(
            {
                "prompt": f"继续第 {idx + 1} 个扩展追问：结合前文说明一个额外风险和一个观测指标，并引用 {task_id}。",
                "expect": [task_id],
            }
        )
    return base + extra


def is_assistant_event(event: dict[str, Any]) -> bool:
    """处理 scripts/long_run_test.py 中的 is assistant event 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    kind = str(event.get("kind", "")).lower()
    return kind in {"assistant", "chat"}


def is_thinking_event(event: dict[str, Any]) -> bool:
    """处理 scripts/long_run_test.py 中的 is thinking event 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    return str(event.get("kind", "")).lower() == "thinking"


def extract_response_text(events: list[dict[str, Any]]) -> str:
    """处理 scripts/long_run_test.py 中的 extract response text 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    parts: list[str] = []
    for event in events:
        if not is_assistant_event(event):
            continue
        text = event.get("text")
        if isinstance(text, str) and text:
            parts.append(text)
    return "".join(parts)


def is_completion_event(event: dict[str, Any]) -> bool:
    """处理 scripts/long_run_test.py 中的 is completion event 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    kind = str(event.get("kind", "")).lower()
    text = str(event.get("text", "")).lower()
    return kind == "run_state" and text in {"idle", "stream finished"}


def is_run_start_event(event: dict[str, Any]) -> bool:
    """处理 scripts/long_run_test.py 中的 is run start event 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    kind = str(event.get("kind", "")).lower()
    text = str(event.get("text", "")).lower()
    return kind == "user" or (kind == "run_state" and text in {"queued", "working"})


def relevant_events(events: list[dict[str, Any]], start_seq: int) -> list[dict[str, Any]]:
    """处理 scripts/long_run_test.py 中的 relevant events 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    out = []
    for event in events:
        if event_seq(event) <= start_seq:
            continue
        kind = str(event.get("kind", "")).lower()
        level = str(event.get("level", "")).lower()
        text = str(event.get("text", ""))
        if kind in {"assistant", "chat", "thinking", "error"} or level in {"error", "fatal"} or is_completion_event(event):
            out.append(event)
    return out


def observe_event_item(summary: Summary, item: dict[str, Any]) -> None:
    """处理 scripts/long_run_test.py 中的 observe event item 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    if str(item.get("kind", "")).lower() != "profile":
        return
    text = item.get("text")
    if not isinstance(text, str) or not text:
        summary.profile_parse_errors += 1
        return
    payload, partial = parse_profile_event_text(text)
    if not payload:
        summary.profile_parse_errors += 1
        return
    if partial:
        summary.profile_partial_events += 1
    summary.observe_profile_payload(payload)


def _regex_json_string(text: str, key: str) -> str | None:
    """处理 scripts/long_run_test.py 中的 regex json string 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    match = re.search(rf'"{re.escape(key)}"\s*:\s*"([^"]*)"', text)
    return match.group(1) if match else None


def _regex_json_int(text: str, key: str) -> int | None:
    """处理 scripts/long_run_test.py 中的 regex json int 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    match = re.search(rf'"{re.escape(key)}"\s*:\s*(-?\d+)', text)
    return int(match.group(1)) if match else None


def _regex_json_bool(text: str, key: str) -> bool | None:
    """处理 scripts/long_run_test.py 中的 regex json bool 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    match = re.search(rf'"{re.escape(key)}"\s*:\s*(true|false)', text)
    if not match:
        return None
    return match.group(1) == "true"


def _partial_cpu_tasks(text: str) -> list[dict[str, Any]]:
    """处理 scripts/long_run_test.py 中的 partial cpu tasks 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    tasks: list[dict[str, Any]] = []
    patterns = [
        re.compile(r'\{"name":"(?P<name>[^"]+)".*?"cpu_pct":(?P<pct>[0-9]+(?:\.[0-9]+)?)'),
        re.compile(r'\{"n":"(?P<name>[^"]+)".*?"p":(?P<pct>[0-9]+(?:\.[0-9]+)?)'),
    ]
    for pattern in patterns:
        for match in pattern.finditer(text):
            try:
                raw = match.group(0)
                task = {"n": match.group("name"), "p": float(match.group("pct"))}
                core = _regex_json_int(raw, "c")
                if core is not None:
                    task["c"] = core
                cp_match = re.search(r'"cp"\s*:\s*([0-9]+(?:\.[0-9]+)?)', raw)
                if cp_match:
                    task["cp"] = float(cp_match.group(1))
                tasks.append(task)
            except ValueError:
                continue
    return tasks


def parse_profile_event_text(text: str) -> tuple[dict[str, Any] | None, bool]:
    """处理 scripts/long_run_test.py 中的 parse profile event text 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    try:
        payload = json.loads(text)
        return (payload, False) if isinstance(payload, dict) else (None, False)
    except json.JSONDecodeError:
        pass

    if not text.startswith("{"):
        return None, True
    phase = _regex_json_string(text, "phase") or _regex_json_string(text, "sdk_event")
    event = _regex_json_string(text, "event")
    if not phase and not event and "\"cpu\"" not in text:
        return None, True
    payload: dict[str, Any] = {
        "event": event or "partial_profile",
        "phase": phase or event or "partial_profile",
        "status": _regex_json_string(text, "status"),
        "sdk_event": _regex_json_string(text, "sdk_event"),
        "correlation_id": _regex_json_string(text, "correlation_id"),
        "session_id": _regex_json_string(text, "session_id"),
        "step": _regex_json_int(text, "step"),
        "elapsed_us": _regex_json_int(text, "elapsed_us"),
        "partial": True,
    }
    if payload["phase"] == "llm.http.attempt" or payload.get("sdk_event") == "llm.http.attempt":
        attrs: dict[str, Any] = {}
        for key in ("stage", "err"):
            value = _regex_json_string(text, key)
            if value is not None:
                attrs[key] = value
        for key in ("http_status", "attempt_id", "attempt_index", "total_ms", "connect_ms", "header_ms", "first_body_ms"):
            value = _regex_json_int(text, key)
            if value is not None:
                attrs[key] = value
        saw_body = _regex_json_bool(text, "saw_body")
        if saw_body is not None:
            attrs["saw_body"] = saw_body
        payload["attributes"] = attrs
    heap: dict[str, Any] = {}
    for key in ("internal_free", "internal_total", "internal_largest", "psram_free", "psram_total", "psram_largest", "free", "min_free"):
        value = _regex_json_int(text, key)
        if value is not None:
            heap[key] = value
    if heap:
        payload["heap"] = heap
    tasks = _partial_cpu_tasks(text)
    if tasks or "\"cpu\"" in text:
        cpu: dict[str, Any] = {"available": True, "tasks": tasks}
        total_pct_match = re.search(r'"total_pct"\s*:\s*([0-9]+(?:\.[0-9]+)?)', text)
        if total_pct_match:
            cpu["total_pct"] = float(total_pct_match.group(1))
        cores = _regex_json_int(text, "cores")
        if cores is not None:
            cpu["cores"] = cores
        core_pct_match = re.search(r'"core_pct"\s*:\s*\[([^\]]*)', text)
        if core_pct_match:
            values: list[float] = []
            for part in core_pct_match.group(1).split(","):
                try:
                    values.append(float(part.strip()))
                except ValueError:
                    pass
            if values:
                cpu["core_pct"] = values
        payload["cpu"] = cpu
    return payload, True


def observe_http_success(summary: Summary, endpoint: str, elapsed_seconds: float, http: HttpClient) -> None:
    """处理 scripts/long_run_test.py 中的 observe http success 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    summary.http_ok += 1
    summary.observe_http_latency(endpoint, elapsed_seconds)
    summary.observe_http_client_timing(endpoint, http.last_timing)


def compact_related_events(events: list[dict[str, Any]], limit: int = 20) -> list[dict[str, Any]]:
    """处理 scripts/long_run_test.py 中的 compact related events 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    related: list[dict[str, Any]] = []
    for event in events:
        if not isinstance(event, dict):
            continue
        kind = str(event.get("kind", "")).lower()
        keep = kind in {"profile", "run_state", "assistant", "chat", "thinking", "error"}
        if not keep:
            continue
        row = {
            "seq": event.get("seq"),
            "kind": event.get("kind"),
            "level": event.get("level"),
        }
        text = event.get("text")
        if kind == "profile" and isinstance(text, str):
            payload, _partial = parse_profile_event_text(text)
            payload = payload or {}
            if isinstance(payload, dict):
                row.update(
                    {
                        "phase": payload.get("phase") or payload.get("sdk_event"),
                        "status": payload.get("status"),
                        "correlation_id": payload.get("correlation_id"),
                        "attributes": payload.get("attributes"),
                        "error": payload.get("error"),
                    }
                )
        elif isinstance(text, str):
            row["text"] = compact_text(text, 240)
        related.append(row)
    return related[-limit:]


def wait_for_message_events(
    http: HttpClient,
    start_seq: int,
    timeout: float,
    poll_interval: float,
    events_writer: JsonlWriter,
    summary: Summary,
    event_limit: int,
) -> tuple[list[dict[str, Any]], int, bool, dict[str, Any]]:
    """处理 scripts/long_run_test.py 中的 wait for message events 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    started = time.monotonic()
    deadline = time.monotonic() + timeout
    latest_seq = start_seq
    collected: list[dict[str, Any]] = []
    timing: dict[str, Any] = {}
    assistant_event_count = 0
    assistant_text_chars = 0
    thinking_event_count = 0
    thinking_text_chars = 0
    error_event_count = 0
    last_runtime_error = None
    saw_run_start = False
    saw_response = False
    saw_completion = False
    while time.monotonic() < deadline:
        payload, _elapsed = http.get_json("/api/events", {"since": latest_seq, "limit": event_limit})
        observe_http_success(summary, "events", _elapsed, http)
        summary.observe_events_payload(payload)
        batch = payload.get("events") if isinstance(payload.get("events"), list) else []
        batch_had_response = False
        for item in batch:
            if isinstance(item, dict):
                seq = event_seq(item)
                latest_seq = max(latest_seq, seq)
                summary.observe_event_seq(seq)
                observe_event_item(summary, item)
                events_writer.write({"ts": utc_now(), "event": item})
                if event_seq(item) > start_seq:
                    collected.append(item)
                    kind = str(item.get("kind", "")).lower()
                    level = str(item.get("level", "")).lower()
                    text = str(item.get("text", ""))
                    elapsed_ms = round((time.monotonic() - started) * 1000, 2)
                    if kind == "run_state" and text.lower() == "queued" and "queued_after_ms" not in timing:
                        timing["queued_after_ms"] = elapsed_ms
                    if kind == "run_state" and text.lower() == "working" and "working_after_ms" not in timing:
                        timing["working_after_ms"] = elapsed_ms
                    if is_run_start_event(item):
                        saw_run_start = True
                    if is_assistant_event(item):
                        assistant_event_count += 1
                        assistant_text_chars += len(text)
                        if "first_token_after_ms" not in timing:
                            timing["first_token_after_ms"] = elapsed_ms
                        if "first_output_after_ms" not in timing:
                            timing["first_output_after_ms"] = elapsed_ms
                    if is_thinking_event(item):
                        thinking_event_count += 1
                        thinking_text_chars += len(text)
                        if "first_thinking_after_ms" not in timing:
                            timing["first_thinking_after_ms"] = elapsed_ms
                        if "first_output_after_ms" not in timing:
                            timing["first_output_after_ms"] = elapsed_ms
                    if level in {"error", "fatal"} or kind == "error":
                        error_event_count += 1
                        last_runtime_error = text
                    if kind == "run_state" and text.lower() == "stream finished" and saw_run_start:
                        timing.setdefault("stream_finished_after_ms", elapsed_ms)
                    if kind == "run_state" and text.lower() == "idle" and saw_run_start:
                        timing.setdefault("idle_after_ms", elapsed_ms)
                    if saw_run_start and (is_assistant_event(item) or kind == "error" or level in {"error", "fatal"}):
                        batch_had_response = True
                    if saw_run_start and is_completion_event(item):
                        saw_completion = True
        latest_seq = max(latest_seq, safe_int(payload.get("latest_seq"), latest_seq))
        if batch_had_response:
            saw_response = True
        if saw_completion and saw_response:
            break
        time.sleep(poll_interval)
    timing["assistant_event_count"] = assistant_event_count
    timing["assistant_text_chars"] = assistant_text_chars
    timing["thinking_event_count"] = thinking_event_count
    timing["thinking_text_chars"] = thinking_text_chars
    timing["error_event_count"] = error_event_count
    timing["last_runtime_error"] = last_runtime_error
    timing["total_elapsed_ms"] = round((time.monotonic() - started) * 1000, 2)
    timing["log_incomplete"] = safe_int(timing.get("assistant_text_chars"), 0) == 0 and saw_response
    return collected, latest_seq, saw_response and saw_completion, timing


def wait_for_interaction_idle(
    http: HttpClient,
    timeout: float,
    poll_interval: float,
    summary: Summary,
    events_writer: JsonlWriter | None = None,
    latest_seq: int | None = None,
    event_limit: int = 120,
) -> tuple[bool, dict[str, Any] | None, int | None]:
    """处理 scripts/long_run_test.py 中的 wait for interaction idle 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    summary.idle_waits += 1
    deadline = time.monotonic() + timeout
    last_status: dict[str, Any] | None = None
    seq = latest_seq
    while time.monotonic() < deadline:
        try:
            status, _elapsed = http.get_json("/api/status")
            observe_http_success(summary, "status", _elapsed, http)
            summary.observe_profile_config(status)
            last_status = flatten_status(status)
            if status_is_idle(last_status):
                return True, last_status, seq
        except Exception as exc:
            summary.fail(str(exc), http=True)

        if events_writer is not None and seq is not None:
            try:
                payload, _elapsed = http.get_json("/api/events", {"since": seq, "limit": event_limit})
                observe_http_success(summary, "events", _elapsed, http)
                summary.observe_events_payload(payload)
                for item in payload.get("events", []):
                    if isinstance(item, dict):
                        item_seq = event_seq(item)
                        seq = max(seq, item_seq)
                        summary.observe_event_seq(item_seq)
                        observe_event_item(summary, item)
                        events_writer.write({"ts": utc_now(), "event": item})
                seq = max(seq, safe_int(payload.get("latest_seq"), seq))
            except Exception as exc:
                summary.fail(str(exc), http=True)
        time.sleep(poll_interval)
    summary.idle_timeouts += 1
    return False, last_status, seq


def flatten_status(status: dict[str, Any]) -> dict[str, Any]:
    """处理 scripts/long_run_test.py 中的 flatten status 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    heap = status.get("heap") if isinstance(status.get("heap"), dict) else {}
    wifi = status.get("wifi") if isinstance(status.get("wifi"), dict) else {}
    events = status.get("events") if isinstance(status.get("events"), dict) else {}
    interaction = status.get("interaction") if isinstance(status.get("interaction"), dict) else {}
    camera = status.get("camera") if isinstance(status.get("camera"), dict) else {}
    usb = status.get("usb") if isinstance(status.get("usb"), dict) else {}
    profile = status.get("profile") if isinstance(status.get("profile"), dict) else {}
    return {
        "uptime_ms": status.get("uptime_ms"),
        "heap_internal_free": heap.get("internal_free"),
        "heap_psram_free": heap.get("psram_free"),
        "wifi_connected": wifi.get("connected"),
        "wifi_sta_ip": wifi.get("sta_ip"),
        "wifi_ap_started": wifi.get("ap_started"),
        "usb_mounted": usb.get("mounted"),
        "camera_frames": camera.get("frames"),
        "camera_drops": camera.get("drops"),
        "events_lifecycle": events.get("lifecycle"),
        "events_dropped": events.get("dropped"),
        "events_ring_overwrites": events.get("ring_overwrites"),
        "events_sink_dropped": events.get("sink_dropped"),
        "events_published": events.get("published"),
        "events_maintenance_skipped": events.get("maintenance_skipped"),
        "events_oom_dropped": events.get("oom_dropped"),
        "events_usb_queue_dropped": events.get("usb_queue_dropped"),
        "events_display_queue_dropped": events.get("display_queue_dropped"),
        "events_max_usb_queue_waiting": events.get("max_usb_queue_waiting"),
        "events_max_display_queue_waiting": events.get("max_display_queue_waiting"),
        "events_oldest_seq": events.get("oldest_seq"),
        "events_capacity": events.get("capacity"),
        "events_usb_log_started": events.get("usb_log_started"),
        "interaction_state": interaction.get("state"),
        "interaction_busy": interaction.get("busy"),
        "interaction_pending": interaction.get("pending"),
        "profile_enabled": profile.get("enabled"),
        "profile_level": profile.get("level"),
        "profile_samples": profile.get("samples"),
        "profile_dropped": profile.get("dropped"),
    }


def write_summary(path: Path, summary: Summary, finished: bool = False) -> None:
    """处理 scripts/long_run_test.py 中的 write summary 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    if finished:
        summary.finished_at = utc_now()
    path.write_text(json.dumps(summary.as_dict(), ensure_ascii=False, indent=2, sort_keys=True), encoding="utf-8")


def init_summary(args: argparse.Namespace, output: Path) -> Summary:
    """处理 scripts/long_run_test.py 中的 init summary 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    summary = Summary()
    summary.run_id = args.run_id
    summary.run_label = args.run_label
    summary.output_dir = str(output)
    summary.requested_duration_seconds = args.duration_seconds
    summary.http_persistent_enabled = bool(args.persistent_http)
    summary.profile_expected_level = None if args.profile_level == "keep" else args.profile_level
    summary.profile_expected_interval_ms = args.profile_interval_ms if args.profile_interval_ms >= 0 else None
    return summary


def finalize_summary_state(summary: Summary, elapsed_seconds: float) -> None:
    """处理 scripts/long_run_test.py 中的 finalize summary state 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    summary.elapsed_seconds = round(elapsed_seconds, 3)
    if summary.stop_reason is None:
        summary.stop_reason = "unknown"
    if summary.stop_reason == "dry_run":
        summary.incomplete_run = False
        return
    if summary.requested_duration_seconds is not None:
        summary.incomplete_run = (
            elapsed_seconds + 1.0 < summary.requested_duration_seconds and
            summary.stop_reason != "duration_complete"
        )
    else:
        summary.incomplete_run = summary.stop_reason not in {"signal", "exception"}


def finalize_timing(timing: dict[str, Any], submit_elapsed_ms: float, total_elapsed_ms: float) -> dict[str, Any]:
    """处理 scripts/long_run_test.py 中的 finalize timing 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    out = dict(timing)
    out["submit_elapsed_ms"] = round(submit_elapsed_ms, 2)
    for key in [
        "queued_after_ms",
        "working_after_ms",
        "first_output_after_ms",
        "first_thinking_after_ms",
        "first_token_after_ms",
        "stream_finished_after_ms",
        "idle_after_ms",
    ]:
        value = out.get(key)
        if isinstance(value, (int, float)):
            out[key] = round(float(value) + submit_elapsed_ms, 2)
    out["total_elapsed_ms"] = round(total_elapsed_ms, 2)
    return out


def apply_profile_guard(http: HttpClient, summary: Summary, args: argparse.Namespace) -> list[dict[str, Any]]:
    """处理 scripts/long_run_test.py 中的 apply profile guard 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    if args.no_profile_guard:
        return []
    commands = ["/profile on"]
    if args.profile_level != "keep":
        commands.append(f"/profile level {args.profile_level}")
    if args.profile_interval_ms >= 0:
        commands.append(f"/profile interval {args.profile_interval_ms}")
    results: list[dict[str, Any]] = []
    for command in commands:
        row: dict[str, Any] = {"command": command}
        try:
            elapsed = http.post_message(command)
            observe_http_success(summary, "messages", elapsed, http)
            row["elapsed_ms"] = round(elapsed * 1000.0, 2)
            row["ok"] = True
        except Exception as exc:
            summary.fail(str(exc), http=True)
            row["ok"] = False
            row["error"] = str(exc)
        results.append(row)
    return results


def run_fault_ota_reject_probe(http: HttpClient, summary: Summary, args: argparse.Namespace) -> dict[str, Any]:
    """处理 scripts/long_run_test.py 中的 run fault ota reject probe 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    row: dict[str, Any] = {
        "ts": utc_now(),
        "fault": "ota_reject",
        "name": args.fault_ota_name,
        "size": args.fault_ota_size,
    }
    try:
        code, body, elapsed = http.ota_prepare_probe(args.fault_ota_name, args.fault_ota_size)
        observe_http_success(summary, "ota_prepare_probe", elapsed, http)
        row.update({
            "http_code": code,
            "elapsed_ms": round(elapsed * 1000.0, 2),
            "accepted": 200 <= code < 300,
            "body": compact_text(body, 500),
        })
        if 200 <= code < 300:
            summary.fail("fault_ota_reject was accepted unexpectedly", http=False)
    except Exception as exc:
        summary.fail(str(exc), http=True)
        row.update({"error": str(exc), "trace": traceback.format_exc(limit=3)})
    summary.observe_fault_result(row)
    return row


def run_fault_web_timeout_probe(summary: Summary, args: argparse.Namespace) -> dict[str, Any]:
    """处理 scripts/long_run_test.py 中的 run fault web timeout probe 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    row: dict[str, Any] = {
        "ts": utc_now(),
        "fault": "web_timeout",
        "timeout": args.fault_web_timeout,
    }
    probe = HttpClient(args.host, args.fault_web_timeout, persistent=False)
    try:
        _status, elapsed = probe.get_json("/api/status")
        row.update({"timed_out": False, "elapsed_ms": round(elapsed * 1000.0, 2)})
    except Exception as exc:
        row.update({"timed_out": True, "error": str(exc)})
    finally:
        probe.close()
    summary.observe_fault_result(row)
    return row


def run_config_audit(http: HttpClient, summary: Summary) -> dict[str, Any]:
    """处理 scripts/long_run_test.py 中的 run config audit 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    row: dict[str, Any] = {"ts": utc_now(), "config_audit": True, "risks": []}
    risks: list[str] = []
    try:
        text, elapsed = http.get_config_text()
        observe_http_success(summary, "config", elapsed, http)
        row["elapsed_ms"] = round(elapsed * 1000.0, 2)
        try:
            config = json.loads(text)
        except json.JSONDecodeError:
            risks.append("config_not_json")
            config = {}
        if isinstance(config, dict):
            raw = json.dumps(config, ensure_ascii=False).lower()
            if "sk-" in raw or "api_key" in raw or "authorization" in raw:
                risks.append("possible_secret_material_present")
            io = config.get("io") if isinstance(config.get("io"), dict) else {}
            web = io.get("web") if isinstance(io.get("web"), dict) else {}
            if web.get("enabled") is True and not web.get("auth"):
                risks.append("web_console_auth_not_configured")
            runtime = config.get("runtime") if isinstance(config.get("runtime"), dict) else {}
            if runtime.get("tls_verify") is False or runtime.get("verify_tls") is False:
                risks.append("tls_verification_disabled")
            tools = config.get("tools") if isinstance(config.get("tools"), dict) else {}
            if tools.get("lua") is True or tools.get("file") is True:
                risks.append("debug_tools_enabled")
        row["risks"] = risks
    except Exception as exc:
        summary.fail(str(exc), http=True)
        risks.append("config_audit_failed")
        row.update({"risks": risks, "error": str(exc)})
    summary.observe_config_audit(risks)
    return row


def build_status_line(
    summary: Summary,
    started_mono: float,
    latest_status: dict[str, Any] | None,
    output_dir: Path,
) -> str:
    """处理 scripts/long_run_test.py 中的 build status line 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    elapsed = format_duration(time.monotonic() - started_mono)
    status = latest_status or {}
    heap = status.get("heap_internal_free", "?")
    psram = status.get("heap_psram_free", "?")
    wifi = status.get("wifi_connected", "?")
    state = status.get("interaction_state", "?")
    busy = status.get("interaction_busy", "?")
    pending = status.get("interaction_pending", "?")
    dropped = status.get("events_dropped", summary.max_events_dropped)
    profile_samples = status.get("profile_samples", summary.profile_events)
    return (
        f"[{utc_now()}] elapsed={elapsed} loops={summary.loops} "
        f"http_ok={summary.http_ok} http_err={summary.http_errors} "
        f"serial_ok={summary.serial_writes} serial_err={summary.serial_write_failures + summary.serial_errors} "
        f"short={summary.short_conversations}/{summary.short_conversation_failures} "
        f"long_turns={summary.long_conversation_turns} ctx_fail={summary.context_check_failures} "
        f"skipped={summary.skipped_commands} idle_waits={summary.idle_waits}/{summary.idle_timeouts} "
        f"wifi={wifi} state={state} device_busy={busy} device_pending={pending} heap={heap} psram={psram} "
        f"dropped={dropped} profile={profile_samples} out={output_dir}"
    )


def build_final_report(summary: Summary, output_dir: Path, elapsed_seconds: float) -> str:
    """处理 scripts/long_run_test.py 中的 build final report 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    total_short = summary.short_conversations + summary.short_conversation_failures
    short_ok_rate = 100.0 if total_short == 0 else (summary.short_conversations / total_short) * 100.0
    total_long = summary.long_conversations
    long_ok = max(0, total_long - summary.long_conversation_failures)
    long_ok_rate = 100.0 if total_long == 0 else (long_ok / total_long) * 100.0
    timing_samples = summary.conversation_timing_samples
    first_output_samples = len(summary.first_output_samples_ms)
    first_thinking_samples = len(summary.first_thinking_samples_ms)
    first_token_samples = len(summary.first_token_samples_ms)
    avg_first_output = (
        "n/a" if first_output_samples == 0 else f"{summary.first_output_total_ms / first_output_samples:.1f} ms"
    )
    avg_first_thinking = (
        "n/a" if first_thinking_samples == 0 else f"{summary.first_thinking_total_ms / first_thinking_samples:.1f} ms"
    )
    avg_first_token = (
        "n/a" if first_token_samples == 0 else f"{summary.first_token_total_ms / first_token_samples:.1f} ms"
    )
    avg_total_elapsed = (
        "n/a" if timing_samples == 0 else f"{summary.total_elapsed_total_ms / timing_samples:.1f} ms"
    )
    max_total_elapsed = "n/a" if timing_samples == 0 else f"{summary.max_total_elapsed_ms:.1f} ms"
    first_output_p50 = percentile(summary.first_output_samples_ms, 50)
    first_output_p95 = percentile(summary.first_output_samples_ms, 95)
    first_thinking_p50 = percentile(summary.first_thinking_samples_ms, 50)
    first_thinking_p95 = percentile(summary.first_thinking_samples_ms, 95)
    first_token_p50 = percentile(summary.first_token_samples_ms, 50)
    first_token_p95 = percentile(summary.first_token_samples_ms, 95)
    total_elapsed_p50 = percentile(summary.total_elapsed_samples_ms, 50)
    total_elapsed_p95 = percentile(summary.total_elapsed_samples_ms, 95)
    latency_lines = []
    for endpoint in sorted(summary.http_latency_ms):
        latency_lines.append(f"  {endpoint}: {format_latency_summary(summary.http_latency_ms[endpoint])}")
    if not latency_lines:
        latency_lines.append("  none")
    dns_latency_lines = []
    for endpoint in sorted(summary.http_dns_latency_ms):
        dns_latency_lines.append(f"  {endpoint}: {format_latency_summary(summary.http_dns_latency_ms[endpoint])}")
    if not dns_latency_lines:
        dns_latency_lines.append("  none")
    request_latency_lines = []
    for endpoint in sorted(summary.http_request_latency_ms):
        request_latency_lines.append(f"  {endpoint}: {format_latency_summary(summary.http_request_latency_ms[endpoint])}")
    if not request_latency_lines:
        request_latency_lines.append("  none")
    read_latency_lines = []
    for endpoint in sorted(summary.http_read_latency_ms):
        read_latency_lines.append(f"  {endpoint}: {format_latency_summary(summary.http_read_latency_ms[endpoint])}")
    if not read_latency_lines:
        read_latency_lines.append("  none")
    error_category_lines = [
        f"  {key}: {value}"
        for key, value in sorted(summary.http_error_categories.items())
    ] or ["  none"]
    failure_reason_lines = [
        f"  {key}: {value}"
        for key, value in sorted(summary.conversation_failure_reasons.items())
    ] or ["  none"]
    ota_failure_lines = [
        f"  {key}: {value}"
        for key, value in sorted(summary.ota_failure_counts.items())
    ] or ["  none"]
    fault_lines = [
        f"  {json.dumps(item, ensure_ascii=False, sort_keys=True)}"
        for item in summary.fault_injection_results[:20]
    ] or ["  none"]
    config_risk_lines = [f"  {risk}" for risk in summary.config_audit_risks] or ["  none"]
    profile_phase_lines: list[str] = []
    for phase, count in sorted(summary.profile_phase_counts.items()):
        total_us = summary.profile_phase_total_us.get(phase, 0.0)
        avg_ms = "n/a" if count <= 0 or total_us <= 0 else f"{(total_us / count) / 1000.0:.1f} ms"
        max_us = summary.profile_phase_max_us.get(phase, 0.0)
        max_ms = "n/a" if max_us <= 0 else f"{max_us / 1000.0:.1f} ms"
        profile_phase_lines.append(f"  {phase}: count={count}, avg={avg_ms}, max={max_ms}")
    if not profile_phase_lines:
        profile_phase_lines.append("  none")
    profile_mismatch_lines = [
        f"  {json.dumps(item, ensure_ascii=False, sort_keys=True)}"
        for item in summary.profile_mismatches[:10]
    ] or ["  none"]
    llm_attempt_stage_lines = [
        f"  profile {key}: {value}"
        for key, value in sorted(summary.llm_http_attempt_stage_counts.items())
    ] or ["  profile none"]
    llm_attempt_pool_lines = [
        f"  pool {key}: {value}"
        for key, value in sorted(summary.llm_http_attempt_pool_counts.items())
    ] or ["  pool none"]
    llm_attempt_discard_lines = [
        f"  discard {key}: {value}"
        for key, value in sorted(summary.llm_http_attempt_discard_counts.items())
    ] or ["  discard none"]
    serial_attempt_stage_lines = [
        f"  serial {key}: {value}"
        for key, value in sorted(summary.serial_http_attempt_stage_counts.items())
    ] or ["  serial none"]
    cpu_task_rows = []
    for name in sorted(summary.profile_cpu_task_samples,
                       key=lambda item: summary.profile_cpu_task_max_pct.get(item, 0.0),
                       reverse=True)[:12]:
        samples = summary.profile_cpu_task_samples.get(name, 0)
        avg = summary.profile_cpu_task_total_pct.get(name, 0.0) / max(1, samples)
        cpu_task_rows.append(
            f"  {name}: samples={samples}, avg={avg:.2f}%, "
            f"max={summary.profile_cpu_task_max_pct.get(name, 0.0):.2f}%, "
            f"last={summary.profile_cpu_task_last_pct.get(name, 0.0):.2f}%"
        )
    if not cpu_task_rows:
        cpu_task_rows.append("  none")
    cpu_total_avg = summary.profile_cpu_total_sum_pct / max(1, summary.profile_cpu_total_samples)
    cpu_core_rows = []
    for core in sorted(summary.profile_cpu_core_samples):
        samples = summary.profile_cpu_core_samples.get(core, 0)
        avg = summary.profile_cpu_core_total_pct.get(core, 0.0) / max(1, samples)
        cpu_core_rows.append(
            f"  CPU{core}: samples={samples}, avg={avg:.2f}%, "
            f"max={summary.profile_cpu_core_max_pct.get(core, 0.0):.2f}%, "
            f"last={summary.profile_cpu_core_last_pct.get(core, 0.0):.2f}%"
        )
    if not cpu_core_rows:
        cpu_core_rows.append("  none")
    attempt_mismatch = summary.serial_http_attempt_failed != summary.llm_http_attempt_error
    lines = [
        "C-Claw long-run test summary",
        f"Run ID: {summary.run_id or 'unknown'}",
        f"Run label: {summary.run_label or 'none'}",
        f"Started: {summary.started_at}",
        f"Finished: {summary.finished_at or utc_now()}",
        f"Elapsed: {format_duration(elapsed_seconds)}",
        f"Requested duration: {format_duration(summary.requested_duration_seconds)}",
        f"Stop reason: {summary.stop_reason or 'unknown'}",
        f"Signal: {summary.signal or 'none'}",
        f"Incomplete run: {str(summary.incomplete_run).lower()}",
        f"Output: {output_dir}",
        "",
        "Counters:",
        f"  Loops: {summary.loops}",
        f"  HTTP OK/errors: {summary.http_ok}/{summary.http_errors}",
        f"  Serial writes/failures/errors: {summary.serial_writes}/{summary.serial_write_failures}/{summary.serial_errors}",
        f"  Skipped slow commands: {summary.skipped_commands}",
        f"  Idle waits/timeouts: {summary.idle_waits}/{summary.idle_timeouts}",
        f"  Conversation timeouts: {summary.conversation_timeouts}",
        f"  Events seen: {summary.events_seen}, unique seqs: {len(summary._seen_event_seqs)}, max seq: {summary.max_event_seq}, latest reported: {summary.event_latest_seq_reported}",
        f"  Event seq duplicate/backward/gap lost: {summary.event_duplicate_seq}/{summary.event_backward_seq}/{summary.event_seq_gap_lost} across {summary.event_seq_gap_count} gaps",
        f"  Event dropped/ring overwrites/sink dropped: {summary.max_events_dropped}/{summary.max_events_ring_overwrites}/{summary.max_events_sink_dropped}",
        "  Ring overwrites are device-side old event replacements; Python event loss is indicated by seq gap lost.",
        f"  Event oldest seq/capacity: {summary.min_events_oldest_seq}/{summary.max_events_capacity}",
        f"  Reboots observed: {summary.reboots_observed}",
        "",
        "HTTP latency:",
        f"  Persistent HTTP: {str(summary.http_persistent_enabled).lower()}, connection rebuilds: {summary.http_connection_rebuilds}",
        *latency_lines,
        "HTTP DNS/mDNS latency:",
        *dns_latency_lines,
        "HTTP request/connect/header latency:",
        *request_latency_lines,
        "HTTP response read latency:",
        *read_latency_lines,
        "",
        "HTTP error categories:",
        *error_category_lines,
        "",
        "Config/security audit:",
        f"  Checked: {str(summary.config_audit_checked).lower()}",
        "  Risks:",
        *config_risk_lines,
        "",
        "Conversations:",
        f"  Short conversations OK/failures: {summary.short_conversations}/{summary.short_conversation_failures} ({short_ok_rate:.1f}% OK)",
        f"  Long conversations OK/failures: {long_ok}/{summary.long_conversation_failures} ({long_ok_rate:.1f}% OK)",
        f"  Long conversation turns completed: {summary.long_conversation_turns}",
        f"  Context check failures: {summary.context_check_failures}",
        f"  Timing samples: {timing_samples}",
        f"  Avg first output: {avg_first_output}",
        f"  First output p50/p95: {'n/a' if first_output_p50 is None else f'{first_output_p50:.1f} ms'} / {'n/a' if first_output_p95 is None else f'{first_output_p95:.1f} ms'}",
        f"  Avg first thinking: {avg_first_thinking}",
        f"  First thinking p50/p95: {'n/a' if first_thinking_p50 is None else f'{first_thinking_p50:.1f} ms'} / {'n/a' if first_thinking_p95 is None else f'{first_thinking_p95:.1f} ms'}",
        f"  Avg first assistant token: {avg_first_token}",
        f"  First assistant token p50/p95: {'n/a' if first_token_p50 is None else f'{first_token_p50:.1f} ms'} / {'n/a' if first_token_p95 is None else f'{first_token_p95:.1f} ms'}",
        f"  Avg total elapsed: {avg_total_elapsed}",
        f"  Total elapsed p50/p95: {'n/a' if total_elapsed_p50 is None else f'{total_elapsed_p50:.1f} ms'} / {'n/a' if total_elapsed_p95 is None else f'{total_elapsed_p95:.1f} ms'}",
        f"  Max total elapsed: {max_total_elapsed}",
        "  Failure reasons:",
        *failure_reason_lines,
        "",
        "Resources:",
        f"  Minimum internal heap: {summary.min_internal_heap}",
        f"  Minimum PSRAM heap: {summary.min_psram_heap}",
        "",
        "OTA:",
        f"  OTA status samples: {summary.ota_status_samples}",
        f"  Last state/manager/failure: {summary.ota_last_state or 'n/a'}/{summary.ota_last_manager_state or 'n/a'}/{summary.ota_last_failure_code or 'n/a'}",
        f"  Rollback pending seen: {str(summary.ota_rollback_pending_seen).lower()}",
        f"  OTA resource min largest PSRAM/internal: {summary.ota_min_psram_largest}/{summary.ota_min_internal_largest}",
        f"  OTA max declared expected size observed: {summary.ota_max_expected_size}",
        "  OTA failure codes:",
        *ota_failure_lines,
        "  Fault injection probes:",
        *fault_lines,
        "",
        "Firmware profile:",
        f"  Profile events: {summary.profile_events}, partial events: {summary.profile_partial_events}, parse errors: {summary.profile_parse_errors}",
        f"  Profile minimum internal heap: {summary.profile_min_internal_heap}",
        f"  Profile minimum PSRAM heap: {summary.profile_min_psram_heap}",
        f"  Profile minimum largest PSRAM block: {summary.profile_min_psram_largest}",
        f"  Profile minimum task stack high-water words: {summary.profile_min_stack_words}",
        f"  Expected profile level/interval_ms: {summary.profile_expected_level or 'keep'}/{summary.profile_expected_interval_ms if summary.profile_expected_interval_ms is not None else 'keep'}",
        f"  Profile config mismatches: {summary.profile_mismatch_count}",
        "  Mismatch samples:",
        *profile_mismatch_lines,
        f"  LLM HTTP attempts profile ok/error: {summary.llm_http_attempt_ok}/{summary.llm_http_attempt_error}",
        f"  Serial HTTP attempt failed: {summary.serial_http_attempt_failed}",
        f"  LLM attempt consistency mismatch: {str(attempt_mismatch).lower()}",
        f"  Serial failed attempt ids: {sorted(summary.serial_http_attempt_failed_ids)}",
        f"  Serial failed attempt ids missing from profile: {sorted(attempt_id for attempt_id in summary.serial_http_attempt_failed_ids if attempt_id not in summary.llm_http_attempt_ids)}",
        "  LLM HTTP attempt stages:",
        *llm_attempt_stage_lines,
        "  LLM HTTP connection pools:",
        *llm_attempt_pool_lines,
        "  LLM HTTP discard reasons:",
        *llm_attempt_discard_lines,
        f"  LLM HTTP retry allowed/performed: {summary.llm_http_attempt_retry_allowed}/{summary.llm_http_attempt_retry_performed}",
        f"  LLM HTTP partial stream/saw done/max reuse reached: {summary.llm_http_attempt_partial_stream}/{summary.llm_http_attempt_saw_done}/{summary.llm_http_attempt_max_reuse_reached}",
        *serial_attempt_stage_lines,
        "  Phases:",
        *profile_phase_lines,
        "  CPU:",
        f"  CPU samples available/unavailable: {summary.profile_cpu_available_samples}/{summary.profile_cpu_unavailable_samples}",
        f"  Legacy total CPU samples/avg/max/last: {summary.profile_cpu_total_samples}/{cpu_total_avg:.2f}%/{summary.profile_cpu_total_max_pct:.2f}%/{summary.profile_cpu_total_last_pct if summary.profile_cpu_total_last_pct is not None else 'n/a'}",
        "  CPU core totals:",
        *cpu_core_rows,
        "  Top task samples:",
        *cpu_task_rows,
        "",
        f"Last error: {summary.last_error or 'none'}",
        "",
        "Artifacts:",
        f"  {output_dir / 'run_status.jsonl'}",
        f"  {output_dir / 'events.jsonl'}",
        f"  {output_dir / 'serial.log'}",
        f"  {output_dir / 'conversation.jsonl'}",
        f"  {output_dir / 'long_conversation.jsonl'}",
        f"  {output_dir / 'summary.json'}",
    ]
    return "\n".join(lines) + "\n"


def run_dry(args: argparse.Namespace) -> int:
    """处理 scripts/long_run_test.py 中的 run dry 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    output = resolve_output_dir(Path(args.output_dir), args.append, args.run_id)
    output.mkdir(parents=True, exist_ok=True)
    summary = init_summary(args, output)
    summary.stop_reason = "dry_run"
    writers = [
        JsonlWriter(output / "run_status.jsonl", append=args.append),
        JsonlWriter(output / "events.jsonl", append=args.append),
        JsonlWriter(output / "conversation.jsonl", append=args.append),
        JsonlWriter(output / "long_conversation.jsonl", append=args.append),
    ]
    writers[0].write({
        "ts": utc_now(),
        "dry_run": True,
        "run_id": args.run_id,
        "run_label": args.run_label,
        "host": args.host,
        "serial_port": args.serial_port,
        "test_items": sorted(args.test_items_set),
    })
    writers[1].write({"ts": utc_now(), "dry_run": True, "events": []})
    writers[2].write({"ts": utc_now(), "dry_run": True, "prompt": SHORT_PROMPTS[0]})
    writers[3].write({"ts": utc_now(), "dry_run": True, "turns": make_long_conversation(args.long_conversation_turns)})
    for writer in writers:
        writer.close()
    (output / "serial.log").write_text("dry-run: serial disabled\n", encoding="utf-8")
    finalize_summary_state(summary, 0)
    write_summary(output / "summary.json", summary, finished=True)
    report = build_final_report(summary, output, 0)
    (output / "summary_report.txt").write_text(report, encoding="utf-8")
    print(report, end="")
    print(f"dry-run ok: wrote outputs to {output}")
    return 0


def run(args: argparse.Namespace) -> int:
    """处理 scripts/long_run_test.py 中的 run 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    output = resolve_output_dir(Path(args.output_dir), args.append, args.run_id)
    output.mkdir(parents=True, exist_ok=True)
    run_status = JsonlWriter(output / "run_status.jsonl", append=args.append)
    events_log = JsonlWriter(output / "events.jsonl", append=args.append)
    conversations = JsonlWriter(output / "conversation.jsonl", append=args.append)
    long_conversations = JsonlWriter(output / "long_conversation.jsonl", append=args.append)
    summary_path = output / "summary.json"
    report_path = output / "summary_report.txt"
    summary = init_summary(args, output)
    stop_event = threading.Event()
    serial_worker = SerialWorker(args.serial_port, args.baudrate, output / "serial.log", enabled=not args.no_serial)

    def request_stop(_signum: int, _frame: Any) -> None:
        """处理 scripts/long_run_test.py 中的 request stop 逻辑。

        该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
        在实现变化时与项目文档同步维护。
        """
        summary.stop_reason = "signal"
        summary.signal = getattr(signal.Signals(_signum), "name", str(_signum))
        stop_event.set()
        serial_worker.request_close()

    signal.signal(signal.SIGINT, request_stop)
    if hasattr(signal, "SIGTERM"):
        signal.signal(signal.SIGTERM, request_stop)

    http = HttpClient(args.host, args.http_timeout, persistent=args.persistent_http)
    serial_worker.start()

    started_mono = time.monotonic()
    end_at = None if args.duration_seconds is None else time.monotonic() + args.duration_seconds
    latest_seq = 0
    previous_uptime: int | None = None
    latest_status: dict[str, Any] | None = None
    next_short = time.monotonic() + args.conversation_interval if args.conversation_interval > 0 else float("inf")
    next_long = time.monotonic() + args.long_conversation_interval if args.long_conversation_interval > 0 else float("inf")
    next_network_command = time.monotonic()
    next_serial_command = time.monotonic()
    next_status_print = time.monotonic()
    serial_command_index = 0
    network_command_index = 0
    status_enabled = has_item(args, "status")
    events_enabled = has_item(args, "events") or has_item(args, "profile")
    ota_status_enabled = has_item(args, "ota_status")
    config_audit_enabled = has_item(args, "config_audit")
    fault_ota_reject_enabled = has_item(args, "fault_ota_reject")
    fault_web_timeout_enabled = has_item(args, "fault_web_timeout")
    serial_commands_enabled = has_item(args, "serial_commands")
    network_commands_enabled = has_item(args, "network_commands")
    short_conversation_enabled = has_item(args, "short_conversation")
    long_conversation_enabled = has_item(args, "long_conversation")
    status_required = (
        status_enabled or serial_commands_enabled or network_commands_enabled or
        short_conversation_enabled or long_conversation_enabled
    )
    conversation_events_required = short_conversation_enabled or long_conversation_enabled
    if not short_conversation_enabled:
        next_short = float("inf")
    if not long_conversation_enabled:
        next_long = float("inf")
    if not network_commands_enabled:
        next_network_command = float("inf")
    if not serial_commands_enabled:
        next_serial_command = float("inf")

    print(
        "C-Claw long-run test started: "
        f"host={http.base} serial={args.serial_port} baudrate={args.baudrate} "
        f"duration={format_duration(args.duration_seconds)} run_id={args.run_id} output={output}"
    )
    if not serial_worker.available and not args.no_serial:
        print("Serial is not available yet; test will continue with Web Console and record serial errors.", file=sys.stderr)

    profile_guard_results = apply_profile_guard(http, summary, args)
    if profile_guard_results:
        run_status.write({"ts": utc_now(), "profile_guard": profile_guard_results})

    if config_audit_enabled:
        run_status.write(run_config_audit(http, summary))
    if fault_ota_reject_enabled:
        run_status.write(run_fault_ota_reject_probe(http, summary, args))
    if fault_web_timeout_enabled:
        run_status.write(run_fault_web_timeout_probe(summary, args))

    try:
        while not stop_event.is_set():
            if should_stop_for_file(args):
                summary.stop_reason = "user_stop"
                break
            if end_at is not None and time.monotonic() >= end_at:
                summary.stop_reason = "duration_complete"
                break
            loop_started = time.monotonic()
            summary.loops += 1
            row: dict[str, Any] = {"ts": utc_now(), "loop": summary.loops}

            for err_text in serial_worker.recent_errors():
                summary.serial_errors += 1
                summary.last_error = err_text
                row.setdefault("serial_errors", []).append(err_text)

            if status_required:
                try:
                    status, status_elapsed = http.get_json("/api/status")
                    observe_http_success(summary, "status", status_elapsed, http)
                    previous_uptime = summary.observe_status(status, previous_uptime)
                    summary.observe_profile_config(status)
                    row["status"] = flatten_status(status)
                    latest_status = row["status"]
                    row["http_status_elapsed_ms"] = round(status_elapsed * 1000, 2)
                except Exception as exc:
                    summary.fail(str(exc), http=True)
                    row["status_error"] = str(exc)

            if events_enabled or conversation_events_required:
                try:
                    events_payload, events_elapsed = http.get_json("/api/events", {"since": latest_seq, "limit": args.event_limit})
                    observe_http_success(summary, "events", events_elapsed, http)
                    summary.observe_events_payload(events_payload)
                    batch = events_payload.get("events") if isinstance(events_payload.get("events"), list) else []
                    row["events_count"] = len(batch)
                    row["http_events_elapsed_ms"] = round(events_elapsed * 1000, 2)
                    for item in batch:
                        if isinstance(item, dict):
                            seq = event_seq(item)
                            latest_seq = max(latest_seq, seq)
                            summary.observe_event_seq(seq)
                            observe_event_item(summary, item)
                            events_log.write({"ts": utc_now(), "event": item})
                    latest_seq = max(latest_seq, safe_int(events_payload.get("latest_seq"), latest_seq))
                except Exception as exc:
                    summary.fail(str(exc), http=True)
                    row["events_error"] = str(exc)

            if ota_status_enabled:
                try:
                    ota_status, ota_elapsed = http.ota_status()
                    observe_http_success(summary, "ota_status", ota_elapsed, http)
                    summary.observe_ota_status(ota_status)
                    row["ota_status"] = {
                        "state": ota_status.get("state"),
                        "manager_state": ota_status.get("manager_state"),
                        "failure_code": ota_status.get("failure_code"),
                        "failure_stage": ota_status.get("failure_stage"),
                        "rollback_pending": ota_status.get("rollback_pending"),
                        "resource_snapshot": ota_status.get("resource_snapshot"),
                    }
                    row["http_ota_status_elapsed_ms"] = round(ota_elapsed * 1000, 2)
                except Exception as exc:
                    summary.fail(str(exc), http=True)
                    row["ota_status_error"] = str(exc)

            now = time.monotonic()
            device_idle = status_is_idle(latest_status)
            if now >= next_serial_command:
                command = SERIAL_COMMANDS[serial_command_index % len(SERIAL_COMMANDS)]
                serial_command_index += 1
                if is_slow_command(command) and not device_idle:
                    busy, pending = status_busy_pending(latest_status)
                    summary.skipped_commands += 1
                    row["serial_command_skipped"] = command
                    row["slow_command_skipped"] = True
                    row["skip_reason"] = f"device busy={busy} pending={pending}"
                else:
                    if serial_worker.write_line(command):
                        summary.serial_writes += 1
                        row["serial_command"] = command
                    else:
                        summary.serial_write_failures += 1
                        row["serial_command_failed"] = command
                next_serial_command = now + max(args.interval, 1.0)

            if now >= next_network_command:
                command = NETWORK_COMMANDS[network_command_index % len(NETWORK_COMMANDS)]
                network_command_index += 1
                if is_slow_command(command) and not device_idle:
                    busy, pending = status_busy_pending(latest_status)
                    summary.skipped_commands += 1
                    row["network_command_skipped"] = command
                    row["slow_command_skipped"] = True
                    row["network_skip_reason"] = f"device busy={busy} pending={pending}"
                else:
                    try:
                        row["network_command"] = command
                        network_elapsed = http.post_message(command)
                        observe_http_success(summary, "messages", network_elapsed, http)
                        row["network_command_elapsed_ms"] = round(network_elapsed * 1000, 2)
                    except Exception as exc:
                        summary.fail(str(exc), http=True)
                        row["network_command_error"] = str(exc)
                next_network_command = now + max(args.interval * 2, 2.0)

            if now >= next_short:
                prompt = SHORT_PROMPTS[summary.short_conversations % len(SHORT_PROMPTS)]
                start_seq = latest_seq
                started = time.monotonic()
                record = {"ts": utc_now(), "type": "short", "prompt": prompt, "start_seq": start_seq}
                try:
                    if not status_is_idle(latest_status):
                        ok_idle, idle_status, latest_seq = wait_for_interaction_idle(
                            http, args.idle_timeout, args.event_poll_interval, summary, events_log, latest_seq, args.event_limit
                        )
                        latest_status = idle_status or latest_status
                        if not ok_idle:
                            summary.conversation_timeouts += 1
                            raise RuntimeError("device did not become idle before short conversation")
                        start_seq = latest_seq if latest_seq is not None else start_seq
                        record["start_seq"] = start_seq
                    submit_elapsed = http.post_message(prompt)
                    observe_http_success(summary, "messages", submit_elapsed, http)
                    submit_elapsed_ms = round(submit_elapsed * 1000, 2)
                    response_events, latest_seq, ok, timing = wait_for_message_events(
                        http, start_seq, args.conversation_timeout, args.event_poll_interval, events_log, summary, args.event_limit
                    )
                    ok_idle, idle_status, latest_seq = wait_for_interaction_idle(
                        http, args.conversation_timeout, args.event_poll_interval, summary, events_log, latest_seq, args.event_limit
                    )
                    latest_status = idle_status or latest_status
                    if not ok_idle:
                        summary.conversation_timeouts += 1
                        ok = False
                    response_text = extract_response_text(response_events)
                    final_timing = finalize_timing(timing, submit_elapsed_ms, (time.monotonic() - started) * 1000)
                    ok = apply_runtime_error_result(summary, ok, final_timing)
                    if ok_idle and "idle_after_ms" not in final_timing:
                        final_timing["idle_after_ms"] = final_timing["total_elapsed_ms"]
                    summary.observe_conversation_timing(final_timing)
                    record.update(
                        {
                            "ok": ok,
                            "elapsed_ms": round((time.monotonic() - started) * 1000, 2),
                            "events": len(response_events),
                            "response_chars": len(response_text),
                            "response_excerpt": compact_text(response_text),
                            **final_timing,
                        }
                    )
                    summary.short_conversations += 1
                    if not ok:
                        summary.short_conversation_failures += 1
                        reason = classify_conversation_failure(ok, final_timing)
                        summary.observe_conversation_failure(reason)
                        record["failure_reason"] = reason
                except Exception as exc:
                    summary.short_conversation_failures += 1
                    summary.fail(str(exc), http=True)
                    reason = classify_http_error(str(exc)) if "urlopen" in str(exc).lower() else "exception"
                    summary.observe_conversation_failure(reason)
                    record.update({"ok": False, "failure_reason": reason, "error": str(exc), "trace": traceback.format_exc(limit=3)})
                conversations.write(record)
                next_short = time.monotonic() + args.conversation_interval

            if now >= next_long:
                turns = make_long_conversation(args.long_conversation_turns)
                conversation_id = f"long-{datetime.now().strftime('%Y%m%d-%H%M%S')}"
                summary.long_conversations += 1
                long_failed = False
                for idx, turn in enumerate(turns, start=1):
                    if stop_event.is_set() or should_stop_for_file(args):
                        if should_stop_for_file(args):
                            summary.stop_reason = "user_stop"
                        break
                    prompt = turn["prompt"]
                    expected = list(turn.get("expect", []))
                    start_seq = latest_seq
                    started = time.monotonic()
                    record = {
                        "ts": utc_now(),
                        "conversation_id": conversation_id,
                        "turn": idx,
                        "turns_total": len(turns),
                        "prompt": prompt,
                        "expected_tokens": expected,
                        "start_seq": start_seq,
                    }
                    try:
                        if not status_is_idle(latest_status):
                            ok_idle, idle_status, latest_seq = wait_for_interaction_idle(
                                http, args.idle_timeout, args.event_poll_interval, summary, events_log, latest_seq, args.event_limit
                            )
                            latest_status = idle_status or latest_status
                            if not ok_idle:
                                summary.conversation_timeouts += 1
                                raise RuntimeError("device did not become idle before long conversation turn")
                            start_seq = latest_seq if latest_seq is not None else start_seq
                            record["start_seq"] = start_seq
                        submit_elapsed = http.post_message(prompt)
                        observe_http_success(summary, "messages", submit_elapsed, http)
                        submit_elapsed_ms = round(submit_elapsed * 1000, 2)
                        response_events, latest_seq, ok, timing = wait_for_message_events(
                            http, start_seq, args.long_conversation_timeout, args.event_poll_interval, events_log, summary, args.event_limit
                        )
                        ok_idle, idle_status, latest_seq = wait_for_interaction_idle(
                            http, args.long_conversation_timeout, args.event_poll_interval, summary, events_log, latest_seq, args.event_limit
                        )
                        latest_status = idle_status or latest_status
                        if not ok_idle:
                            summary.conversation_timeouts += 1
                            ok = False
                        response_text = extract_response_text(response_events)
                        final_timing = finalize_timing(timing, submit_elapsed_ms, (time.monotonic() - started) * 1000)
                        ok = apply_runtime_error_result(summary, ok, final_timing)
                        if ok_idle and "idle_after_ms" not in final_timing:
                            final_timing["idle_after_ms"] = final_timing["total_elapsed_ms"]
                        summary.observe_conversation_timing(final_timing)
                        log_incomplete = bool(final_timing.get("log_incomplete")) or (
                            final_timing.get("assistant_event_count", 0) > 0 and not response_text
                        )
                        context_ok = (contains_all(response_text, expected) if expected else True) or log_incomplete
                        record.update(
                            {
                                "ok": ok,
                                "context_ok": context_ok,
                                "log_incomplete": log_incomplete,
                                "elapsed_ms": round((time.monotonic() - started) * 1000, 2),
                                "events": len(response_events),
                                "response_chars": len(response_text),
                                "response_excerpt": compact_text(response_text, 800),
                                **final_timing,
                            }
                        )
                        summary.long_conversation_turns += 1
                        if not ok:
                            long_failed = True
                            reason = classify_conversation_failure(ok, final_timing, context_ok)
                            summary.observe_conversation_failure(reason)
                            record["failure_reason"] = reason
                            record["failure_related_events"] = compact_related_events(response_events)
                        if not context_ok:
                            summary.context_check_failures += 1
                            long_failed = True
                            reason = classify_conversation_failure(ok, final_timing, context_ok)
                            summary.observe_conversation_failure(reason)
                            record["failure_reason"] = reason
                            record["failure_related_events"] = compact_related_events(response_events)
                            if len(summary.long_failure_diagnostics) < 20:
                                summary.long_failure_diagnostics.append(
                                    {
                                        "ts": utc_now(),
                                        "conversation_id": conversation_id,
                                        "turn": idx,
                                        "reason": reason,
                                        "response_chars": len(response_text),
                                        "assistant_event_count": final_timing.get("assistant_event_count"),
                                        "assistant_text_chars": final_timing.get("assistant_text_chars"),
                                        "related_events": record["failure_related_events"],
                                    }
                                )
                    except Exception as exc:
                        long_failed = True
                        summary.fail(str(exc), http=True)
                        reason = classify_http_error(str(exc)) if "urlopen" in str(exc).lower() else "exception"
                        summary.observe_conversation_failure(reason)
                        record.update({"ok": False, "context_ok": False, "failure_reason": reason, "error": str(exc), "trace": traceback.format_exc(limit=3)})
                    long_conversations.write(record)
                    time.sleep(max(args.interval, 1.0))
                if long_failed:
                    summary.long_conversation_failures += 1
                next_long = time.monotonic() + args.long_conversation_interval

            recent_serial = serial_worker.recent_lines(20)
            if recent_serial:
                for line in recent_serial:
                    summary.observe_serial_line(line)
                row["serial_recent"] = [compact_text(line, 180) for line in recent_serial[-10:]]
            run_status.write(row)
            summary.elapsed_seconds = round(time.monotonic() - started_mono, 3)
            write_summary(summary_path, summary)

            if args.status_interval > 0 and time.monotonic() >= next_status_print:
                print(build_status_line(summary, started_mono, latest_status, output), flush=True)
                next_status_print = time.monotonic() + args.status_interval

            sleep_for = max(0.0, args.interval - (time.monotonic() - loop_started))
            if sleep_for:
                stop_event.wait(sleep_for)
    except Exception as exc:
        summary.stop_reason = "exception"
        summary.fail(str(exc), http=False)
        raise
    finally:
        if summary.stop_reason is None:
            summary.stop_reason = "signal" if stop_event.is_set() else "loop_exited"
        final_elapsed = time.monotonic() - started_mono
        finalize_summary_state(summary, final_elapsed)
        summary.llm_attempt_consistency_mismatch = (
            summary.serial_http_attempt_failed != summary.llm_http_attempt_error
        )
        http.close()
        for line in serial_worker.recent_lines(10000):
            summary.observe_serial_line(line)
        serial_worker.close()
        run_status.close()
        events_log.close()
        conversations.close()
        long_conversations.close()
        write_summary(summary_path, summary, finished=True)
        report = build_final_report(summary, output, final_elapsed)
        report_path.write_text(report, encoding="utf-8")
        print("\n" + report, end="")

    print(f"long-run finished: loops={summary.loops} output={output}")
    if summary.last_error:
        print(f"last error: {summary.last_error}", file=sys.stderr)
    return 0


def build_parser() -> argparse.ArgumentParser:
    """处理 scripts/long_run_test.py 中的 build parser 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    parser = argparse.ArgumentParser(description="C-Claw serial + Web Console long-running test tool")
    parser.add_argument("--serial-port", default=DEFAULT_SERIAL_PORT)
    parser.add_argument("--baudrate", type=int, default=DEFAULT_BAUDRATE)
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--duration", default="6h", help="Duration such as 10m, 6h, 0/forever")
    parser.add_argument("--interval", type=float, default=5.0, help="Base loop interval in seconds")
    parser.add_argument("--output-dir", default=DEFAULT_OUTPUT_DIR)
    parser.add_argument("--http-timeout", type=float, default=8.0)
    parser.add_argument("--persistent-http", dest="persistent_http", action="store_true", default=True, help="Reuse one HTTP connection for Web Console polling")
    parser.add_argument("--no-persistent-http", dest="persistent_http", action="store_false", help="Use urllib one-request-per-connection mode")
    parser.add_argument("--profile-level", default="basic", choices=["basic", "trace", "keep"], help="Expected firmware profile level; use keep to only report current device setting")
    parser.add_argument("--profile-interval-ms", type=int, default=5000, help="Expected firmware profile sample interval; -1 keeps current device interval")
    parser.add_argument("--no-profile-guard", action="store_true", help="Do not send /profile on, /profile level, or /profile interval before the test")
    parser.add_argument("--conversation-interval", type=float, default=180.0, help="Seconds between short chats; 0 disables")
    parser.add_argument("--long-conversation-interval", type=float, default=900.0, help="Seconds between long chats; 0 disables")
    parser.add_argument("--long-conversation-turns", type=int, default=8)
    parser.add_argument("--conversation-timeout", type=float, default=300.0)
    parser.add_argument("--long-conversation-timeout", type=float, default=360.0)
    parser.add_argument("--idle-timeout", type=float, default=180.0, help="Seconds to wait for busy/pending to clear")
    parser.add_argument("--event-poll-interval", type=float, default=2.0)
    parser.add_argument("--event-limit", type=int, default=1000, help="Max events fetched per /api/events poll")
    parser.add_argument("--status-interval", type=float, default=30.0, help="Seconds between console status lines; 0 disables")
    parser.add_argument("--append", action="store_true", help="Append to an existing output directory instead of creating a fresh run subdirectory")
    parser.add_argument("--test-items", default=",".join(DEFAULT_TEST_ITEMS), help="Comma-separated test items: " + ",".join(sorted(ALL_TEST_ITEMS)))
    parser.add_argument("--fault-ota-name", default="CClaw.bin", help="Filename used by the fault_ota_reject prepare probe")
    parser.add_argument("--fault-ota-size", type=int, default=64 * 1024 * 1024, help="Declared size used by fault_ota_reject; no upload body is sent")
    parser.add_argument("--fault-web-timeout", type=float, default=0.001, help="Timeout used by fault_web_timeout probe")
    parser.add_argument("--stop-file", default=None, help="Stop gracefully when this file exists")
    parser.add_argument("--run-label", default=None, help="Human-readable label stored in summary/report")
    parser.add_argument("--run-id", default=None, help=argparse.SUPPRESS)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--no-serial", action="store_true", help="Disable serial access and only use Web Console")
    return parser


def main(argv: list[str] | None = None) -> int:
    """处理 scripts/long_run_test.py 中的 main 逻辑。

    该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应
    在实现变化时与项目文档同步维护。
    """
    parser = build_parser()
    args = parser.parse_args(argv)
    args.duration_seconds = parse_duration(args.duration)
    args.run_id = args.run_id or make_run_id()
    try:
        args.test_items_set = parse_test_items(args.test_items)
    except ValueError as exc:
        parser.error(str(exc))
    if args.dry_run:
        return run_dry(args)
    return run(args)


if __name__ == "__main__":
    raise SystemExit(main())
