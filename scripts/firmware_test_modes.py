"""Functional coverage and staged boundary search using the current firmware HTTP contract.

Only the main thread owns telemetry, files and summaries. Load workers each own
one HTTP connection and return bounded records through a queue.
"""
from __future__ import annotations

import json
import hashlib
import http.client
import math
import queue
import re
import signal
import statistics
import threading
import time
from pathlib import Path
from typing import Any


FUNCTIONAL_CASES = ["status", "events", "profile", "config_audit", "ota_status",
                    "serial", "lua", "file_io", "lua_file", "display", "llm", "context", "camera", "memory"]
STRESS_AXES = ["http", "commands", "lua_cpu", "lua_memory"]
MANUAL_COVERAGE = ["LCD actual appearance and touch response", "Camera image correctness",
                   "OTA upload/boot/rollback", "Power loss and USB removal recovery",
                   "Wi-Fi disconnect/reconnect", "Configuration save/hot-apply/rollback"]


class StopRun(Exception):
    pass


class CaseFailure(RuntimeError):
    def __init__(self, message, category, **evidence):
        super().__init__(message)
        self.category = category
        self.evidence = evidence


def redact(value: Any) -> Any:
    if isinstance(value, dict):
        return {k: ("[REDACTED]" if v else "") if re.search(r"api_key|password|secret|credential|authorization|ssid", k, re.I)
                else redact(v) for k, v in value.items()}
    if isinstance(value, list):
        return [redact(v) for v in value]
    return value


def stage_plan(axes: list[str], clients: list[int], rate: float) -> list[dict[str, Any]]:
    plan = []
    for axis in axes:
        values = clients if axis in {"http", "commands"} else (
            [1000, 10000, 100000, 1000000, 10000000] if axis == "lua_cpu" else
            [16384, 32768, 65536, 131072, 262144])
        for index, value in enumerate(values, 1):
            plan.append({"axis": axis, "level": index, "clients": value if axis in {"http", "commands"} else 1,
                         "value": value, "per_client_rps": rate})
    return plan


def lua_workload(axis: str, value: int, marker: str) -> str:
    if axis == "lua_cpu":
        return f'/lua local s=0; for i=1,{value} do s=(s+i)%100003 end; print("{marker}",s)'
    if axis == "lua_memory":
        return f'/lua local s=string.rep("x",{value}); print("{marker}",#s)'
    return f'/lua print("{marker}")'


def heap_recovered(before: dict, after: dict, byte_limit: int, ratio: float) -> tuple[bool, dict]:
    deltas = {}
    for key in ("internal_free", "internal_largest", "psram_free", "psram_largest"):
        a, b = before.get(key), after.get(key)
        if isinstance(a, (int, float)) and a > 0 and isinstance(b, (int, float)):
            deltas[key] = {"before": a, "after": b, "drop": a - b,
                           "limit": max(byte_limit, a * ratio)}
    return bool(deltas) and all(v["drop"] <= v["limit"] for v in deltas.values()), deltas


def pressure_error_category(row: dict) -> str:
    category = row.get("error_category")
    if category and category != "other":
        return category
    text = str(row.get("error", "")).lower()
    if "getaddrinfo" in text:
        return "dns_or_mdns"
    if "remote end closed connection without response" in text or "remotedisconnected" in text:
        return "peer_closed"
    if "winerror 10054" in text or "connection reset" in text or "econnreset" in text:
        return "connection_reset"
    return "http_response" if row.get("http_status", 0) > 0 else "transport"


def pressure_response(code: int, text: str, expected: str) -> dict:
    """HTTP errors may be plain text; preserve them before parsing success JSON."""
    row = {"http_status": code, "ok": False}
    excerpt = " ".join(text.split())[:240]
    if not 200 <= code < 300:
        return {**row, "error_category": "http_response", "error": f"HTTP {code}: {excerpt}"}
    try:
        payload = json.loads(text)
    except (TypeError, ValueError):
        return {**row, "error_category": "invalid_response", "error": f"HTTP {code}: invalid JSON: {excerpt}"}
    valid = isinstance(payload, dict) and (
        "heap" in payload if expected == "status" else isinstance(payload.get("events"), list) if expected == "events" else payload.get("queued") is True)
    if not valid:
        return {**row, "error_category": "invalid_response", "error": f"HTTP {code}: unexpected {expected} response: {excerpt}"}
    return {"http_status": code, "ok": True}


def event_history_snapshot(flat: dict) -> dict:
    return {key: flat.get("events_" + key) for key in ("capacity", "published", "ring_overwrites", "retained_count", "retained_bytes")}


def camera_processing_report(samples: list[dict]) -> dict:
    usable = [r for r in samples if isinstance(r.get("status", {}).get("camera_frames"), int)]
    result = {"samples": len(usable), "processed_fps": None,
              "ui_source_updates_per_second": None, "ui_pending_replacements": None, "ui_cache_sync_errors": None,
              "note": "Counters and sampled hashes do not verify LCD colors or displayed FPS. Pink jumps are scene-dependent candidates, not faults; drops include rate limiting."}
    if usable:
        for key in ("input_row_bytes", "stride_bytes", "buffer_bytes", "buffer_count",
                    "integrity_checks", "integrity_mismatches", "pink_jump_candidates",
                    "last_mismatch_frame", "last_pink_frame", "last_pink_before_rgb", "last_pink_after_rgb"):
            result["ui_" + key] = usable[-1]["status"].get("camera_ui_" + key)
    if len(usable) < 2:
        return result
    first, last = usable[0], usable[-1]
    for key in ("integrity_checks", "integrity_mismatches", "pink_jump_candidates"):
        a, b = first["status"].get("camera_ui_" + key), last["status"].get("camera_ui_" + key)
        result["ui_" + key + "_delta"] = b - a if type(a) is int and type(b) is int and b >= a else None
    elapsed = last["elapsed"] - first["elapsed"]
    frames = last["status"]["camera_frames"] - first["status"]["camera_frames"]
    if elapsed > 0 and frames >= 0:
        result.update(processed_fps=round(frames / elapsed, 2), processed_frames=frames, elapsed_seconds=round(elapsed, 3))
        for source, target in (("source_updates", "ui_source_updates_per_second"),
                               ("replaced_pending_frames", "ui_pending_replacements"),
                               ("cache_sync_errors", "ui_cache_sync_errors")):
            a, b = first["status"].get("camera_ui_" + source), last["status"].get("camera_ui_" + source)
            if type(a) is int and type(b) is int and b >= a:
                result[target] = round((b - a) / elapsed, 2) if source == "source_updates" else b - a
    return result


def event_history_warmup_candidate(before: dict, after: dict, drops: dict,
                                   metrics: dict, idle_observed: bool, observation_clean: bool) -> bool:
    capacity, first, last = before.get("capacity"), before.get("published"), after.get("published")
    history_filled = (type(capacity) is int and capacity > 0 and after.get("capacity") == capacity
                      and type(first) is int and 0 <= first < capacity
                      and type(last) is int and last - first >= capacity)
    internal = [drops.get(k) for k in ("internal_free", "internal_largest")]
    psram = [drops.get(k) for k in ("psram_free", "psram_largest")]
    # Failed name lookups submit no device work. They still invalidate the load
    # verdict, but need not prevent an independent, bounded heap warmup repeat.
    dns_only = (metrics.get("errors", 0) > 0 and metrics.get("errors") == metrics.get("dns_errors")
                and metrics.get("transport_errors", 0) == 0 and metrics.get("response_error_rate") == 0)
    return bool(history_filled and idle_observed and observation_clean
                and all(d and d["drop"] <= d["limit"] for d in internal)
                and any(d and d["drop"] > d["limit"] for d in psram)
                and metrics.get("commands_accepted", 0) >= 5 and metrics.get("completion_ratio") == 1
                and (metrics.get("error_rate") == 0 or dns_only) and metrics.get("offered_rate_reached"))


def recovery_failure_reason(row: dict, fallback: str = "recovery_failed") -> str:
    recovery = row.get("recovery", {})
    if recovery.get("idle") and recovery.get("monitoring_restored") and recovery.get("heap_within_threshold") is False:
        return "heap_recovery_failed"
    return fallback


def summarize_requests(records: list[dict], completions: dict[str, float], elapsed: float, target_rps: float) -> dict:
    successful = [r for r in records if r.get("ok")]
    responded = [r for r in records if r.get("http_status", 0) > 0]
    latencies = sorted(r["elapsed_ms"] for r in responded)
    p95 = latencies[min(len(latencies) - 1, math.ceil(len(latencies) * .95) - 1)] if latencies else None
    failures = [r for r in records if not r.get("ok")]
    error_categories = {}
    for row in failures:
        category = pressure_error_category(row)
        error_categories[category] = error_categories.get(category, 0) + 1
    marked = [r for r in successful if r.get("marker")]
    finished = [r for r in marked if r["marker"] in completions]
    end_to_end = sorted(max(0, completions[r["marker"]] - r["started"]) * 1000 for r in finished)
    return {"requests": len(records), "accepted": len(successful), "errors": len(records) - len(successful),
            "error_rate": (len(records) - len(successful)) / len(records) if records else None,
            "error_categories": error_categories,
            "dns_errors": error_categories.get("dns_or_mdns", 0),
            "peer_closed_errors": error_categories.get("peer_closed", 0),
            "connection_reset_errors": error_categories.get("connection_reset", 0),
            "transport_errors": sum(r.get("http_status", 0) == 0 and
                                    r.get("error_category") != "dns_or_mdns" and
                                    "getaddrinfo" not in str(r.get("error", "")).lower() for r in failures),
            "response_error_rate": sum(not r.get("ok") for r in responded) / len(responded) if responded else None,
            "dns_lookup_retries": sum(r.get("timing", {}).get("dns_lookup_failures", 0) for r in records),
            "achieved_rps": len(records) / max(elapsed, .001), "target_rps": target_rps,
            "offered_rate_reached": len(records) / max(elapsed, .001) >= target_rps * .8,
            "http_p95_ms": p95, "commands_accepted": len(marked), "commands_completed": len(finished),
            "completion_ratio": len(finished) / len(marked) if marked else None,
            "completion_p95_ms": end_to_end[min(len(end_to_end) - 1, math.ceil(len(end_to_end) * .95) - 1)] if end_to_end else None,
            "status_counts": {str(code): sum(r.get("http_status") == code for r in records)
                              for code in sorted({r.get("http_status", 0) for r in records})},
            "error_examples": [r.get("error") for r in records if not r.get("ok")][:5]}


def evaluate_stage(metrics: dict, *, p95_limit: float, error_limit: float,
                   delivery_loss: bool, runtime_errors: list[str], rebooted: bool,
                   recovered: bool, complete: bool) -> dict:
    reasons = []
    if not complete or metrics["requests"] < 5:
        reasons.append("insufficient_exposure")
    if not metrics["offered_rate_reached"]:
        reasons.append("offered_rate_not_reached")
    if metrics["error_rate"] is not None and metrics["error_rate"] > error_limit:
        reasons.append("request_errors_or_rejections")
    if metrics["http_p95_ms"] is not None and metrics["http_p95_ms"] > p95_limit:
        reasons.append("http_latency_limit")
    if metrics["completion_ratio"] is not None and metrics["completion_ratio"] < 1 - error_limit:
        reasons.append("accepted_commands_not_completed")
    if delivery_loss:
        reasons.append("telemetry_gap")
    if runtime_errors:
        reasons.append("runtime_errors")
    if rebooted:
        reasons.append("device_reboot")
    if not recovered:
        reasons.append("recovery_failed")
    dns_errors = metrics.get("dns_errors", 0) or metrics.get("error_categories", {}).get("dns_or_mdns", 0)
    if not dns_errors and any("getaddrinfo" in str(e).lower() for e in metrics.get("error_examples", [])):
        dns_errors = 1  # Also diagnose older stage records without per-request categories.
    transport_errors = metrics.get("transport_errors", 0)
    dns_retries = metrics.get("dns_lookup_retries", 0)
    if dns_errors or dns_retries:
        reasons.append("dns_or_mdns_errors")
    if transport_errors:
        reasons.append("transport_errors")
    text = " ".join(runtime_errors + [str(e) for e in metrics.get("error_examples", [])]).lower()
    if "task_watchdog" in text:
        kind = "firmware_task_watchdog"
    elif "timed out after" in text or "not enough memory" in text or "lua memory" in text:
        kind = "firmware_lua_guard"
    elif "esp_err_timeout" in text:
        kind = "firmware_queue_rejection"
    else:
        kind = "observed_system_limit"
    uncertain = bool(dns_errors or dns_retries or transport_errors or delivery_loss or not complete or not metrics["offered_rate_reached"])
    response_error_rate = metrics.get("response_error_rate", None if uncertain else metrics.get("error_rate"))
    direct_failure = rebooted or bool(runtime_errors) or (response_error_rate is not None and response_error_rate > error_limit)
    peer_pressure = (complete and metrics["offered_rate_reached"] and not dns_errors and not dns_retries
                     and metrics.get("accepted", 0) >= 5
                     and (metrics.get("peer_closed_errors", 0) + metrics.get("connection_reset_errors", 0)) / max(1, metrics["requests"]) > error_limit)
    if peer_pressure:
        reasons.append("peer_closed_connections_under_load")
        if not direct_failure:
            kind = "http_connection_pressure"
        direct_failure = True
    verdict = "limit" if direct_failure else "inconclusive" if uncertain or metrics["requests"] < 5 else "limit" if reasons else "pass"
    return {"verdict": verdict, "reasons": reasons, "limit_kind": kind if verdict == "limit" else None,
            "observation_issue": "dns_or_mdns" if dns_errors or dns_retries else "transport" if transport_errors else "telemetry" if delivery_loss else None}


class Session:
    def __init__(self, args, common):
        self.a, self.c = args, common
        self.output = common.resolve_output_dir(Path(args.output_dir), args.append, args.run_id)
        self.output.mkdir(parents=True, exist_ok=True)
        self.summary = common.init_summary(args, self.output)
        self.summary.stress = args.mode == "stress"
        self.http = common.HttpClient(args.host, args.http_timeout, persistent=args.persistent_http)
        if args.mode == "stress":
            # Stabilize the test target for this run. Cached-address connect
            # failure still invalidates the cache and resolves again before I/O.
            self.http.resolver.ttl = float("inf")
        self.serial = common.SerialWorker(args.serial_port, args.baudrate, self.output / "serial.log", enabled=not args.no_serial)
        self.logs = {name: common.JsonlWriter(self.output / (name + ".jsonl"))
                     for name in ("run_status", "events", "functional_cases", "pressure_stages", "pressure_requests", "conversation", "long_conversation")}
        self.started = time.monotonic()
        self.deadline = self.started + args.duration_seconds if args.duration_seconds is not None else float("inf")
        self.stop = threading.Event()
        self.cursor = 0
        self.status: dict = {}
        self.cfg: dict = {}
        self.next_status = self.next_events = 0.0
        self.uptime = None
        self.event_tail: list[dict] = []
        self.completions: dict[str, float] = {}
        self.resource_samples: list[dict] = []
        self.idle_samples: list[dict] = []
        self.cases: list[dict] = []
        self.boundaries: dict[str, dict] = {}
        self.phase = "startup"
        self.unhealthy = 0
        self.health_failures = 0
        self.telemetry_errors = 0
        self.delivery_errors = 0
        self.runtime_errors: list[str] = []
        self.result = "running"
        self.old_signals = {}
        self.next_save = 0.0
        self.report_write_errors = 0
        self.report_sequence = 0
        self.last_report_error = None
        self.last_recovery: dict = {}

    def check(self):
        if self.stop.is_set() or self.c.should_stop_for_file(self.a):
            self.summary.stop_reason = self.summary.stop_reason or "user_stop"
            raise StopRun()
        if time.monotonic() >= self.deadline:
            self.summary.stop_reason = "duration_complete"
            raise StopRun()

    def signal_stop(self, sig, frame):
        self.summary.stop_reason = "signal"
        self.stop.set()

    def json_request(self, method, path, body=None, query=None):
        self.check()
        code, text, elapsed = self.http.request_text(method, path, body=body, query=query)
        self.summary.observe_http_latency(path, elapsed)
        if code < 200 or code >= 300:
            raise RuntimeError(f"{path}: HTTP {code}: {self.c.compact_text(text, 200)}")
        self.summary.http_ok += 1
        return text

    def tick(self, force=False):
        self.check()
        now = time.monotonic()
        if force or now >= self.next_status:
            self.next_status = now + self.a.sample_interval
            try:
                status = json.loads(self.json_request("GET", "/api/status"))
                if not isinstance(status, dict) or not isinstance(status.get("heap"), dict):
                    raise ValueError("status is missing heap telemetry")
                self.status = status
                self.uptime = self.summary.observe_status(status, self.uptime)
                self.summary.observe_profile_config(status)
                row = {"ts": self.c.utc_now(), "elapsed": now - self.started,
                       "phase": self.phase, "heap": status["heap"], "status": self.c.flatten_status(status)}
                self.resource_samples.append(row)
                self.logs["run_status"].write(row)
                self.summary.stress_status_samples += int(self.a.mode == "stress")
                self.unhealthy = 0
                free = status["heap"].get("internal_free")
                if self.a.mode == "stress" and isinstance(free, (int, float)) and free < self.a.min_internal_free:
                    self.summary.stop_reason = "internal_heap_floor"
                    raise StopRun()
            except (ValueError, RuntimeError, OSError) as exc:
                self.unhealthy += 1
                self.health_failures += 1
                self.summary.fail(str(exc), http=True)
                self.logs["run_status"].write({"ts": self.c.utc_now(), "phase": self.phase, "status_error": str(exc)})
                self.status = {}
                if self.unhealthy >= self.a.unhealthy_limit:
                    self.summary.stop_reason = "device_unreachable"
                    raise StopRun()
        if force or now >= self.next_events:
            self.next_events = now + self.a.event_poll_interval
            try:
                payload = json.loads(self.json_request("GET", "/api/events", query={"since": self.cursor, "limit": self.a.event_limit}))
                if not isinstance(payload, dict) or not isinstance(payload.get("events"), list):
                    raise ValueError("invalid event payload")
                self.summary.observe_events_payload(payload)
                if any(payload.get(k) for k in ("gap", "truncated", "cursor_reset")):
                    self.delivery_errors += 1
                    self.logs["events"].write({"ts": self.c.utc_now(), "delivery": {k: payload.get(k) for k in ("gap", "truncated", "omitted", "cursor_reset")}})
                for event in payload["events"]:
                    if not isinstance(event, dict):
                        continue
                    self.summary.observe_event_seq(self.c.event_seq(event))
                    self.c.observe_event_item(self.summary, event)
                    self.logs["events"].write({"ts": self.c.utc_now(), "event": event})
                    self.event_tail.append(event)
                    text = str(event.get("text", ""))
                    if event.get("kind") in {"tool", "status", "assistant"}:
                        for marker in re.findall(r"CCT_[A-Za-z0-9_]+", text):
                            self.completions.setdefault(marker, time.monotonic())
                    if event.get("kind") == "error" or event.get("level") in {"error", "fatal"}:
                        self.runtime_errors.append(self.c.compact_text(text, 300))
                self.event_tail = self.event_tail[-4000:]
                self.cursor = self.c.event_cursor(payload, self.cursor)
            except (ValueError, RuntimeError, OSError) as exc:
                self.telemetry_errors += 1
                self.summary.fail(str(exc), http=True)
                self.logs["events"].write({"ts": self.c.utc_now(), "error": str(exc)})
        for line in self.serial.recent_lines(1000):
            self.summary.observe_serial_line(line)
            if "task_wdt: Task watchdog got triggered" in line:
                self.runtime_errors.append("task_watchdog: " + self.c.compact_text(line, 300))
        for error in self.serial.recent_errors():
            self.summary.serial_errors += 1
            self.summary.last_error = error
        self.save()

    def startup_read(self, path, query=None):
        # Retry only read-only startup requests, never accepted model/tool work.
        for attempt in range(3):
            try:
                return json.loads(self.json_request("GET", path, query=query))
            except OSError as exc:
                self.summary.fail(str(exc), http=True)
                if attempt == 2:
                    self.summary.stop_reason = "startup_connection_failed"
                    raise StopRun() from exc
                self.http.close()
                self.check()
                time.sleep(.25 * (attempt + 1))

    def wait(self, seconds):
        end = min(self.deadline, time.monotonic() + seconds)
        while time.monotonic() < end:
            self.tick()
            self.stop.wait(min(.1, max(0, end - time.monotonic())))
        self.check()

    def recover(self, seconds):
        self.phase = "recovery"
        errors_before = (self.health_failures, self.telemetry_errors)
        self.wait(seconds)
        final_errors_before = (self.health_failures, self.telemetry_errors)
        self.tick(force=True)
        restored = final_errors_before == (self.health_failures, self.telemetry_errors)
        idle = self.c.status_is_idle(self.c.flatten_status(self.status))
        self.last_recovery = {"idle": idle, "monitoring_restored": restored,
                              "status_errors": self.health_failures - errors_before[0],
                              "event_request_errors": self.telemetry_errors - errors_before[1]}
        if idle and restored:
            self.idle_samples.append({"elapsed": time.monotonic() - self.started, "heap": dict(self.status.get("heap", {})),
                                      "event_history": event_history_snapshot(self.c.flatten_status(self.status))})
        return idle and restored

    def command(self, text, marker=None, serial=False, timeout=15):
        self.await_idle()
        seq, loss, errors = self.cursor, self.delivery_errors, len(self.runtime_errors)
        if serial:
            if not self.serial.write_line(text):
                raise RuntimeError("serial command write failed")
            self.summary.serial_writes += 1
        else:
            response = json.loads(self.json_request("POST", "/api/messages", text))
            if response.get("queued") is not True:
                raise RuntimeError("message was not queued")
        until = time.monotonic() + timeout
        while time.monotonic() < until:
            self.wait(.1)
            matching = [e for e in self.event_tail if self.c.event_seq(e) > seq and e.get("kind") in {"tool", "status"}
                        and (not marker or marker in str(e.get("text", "")))]
            if len(self.runtime_errors) > errors:
                raise RuntimeError(self.runtime_errors[-1])
            if self.delivery_errors != loss:
                raise RuntimeError("event history incomplete during command")
            if matching:
                return {"event": matching[-1], "evidence": "device tool/status event (not command echo)"}
        raise RuntimeError("command accepted but completion event not observed before timeout")

    def _tool_execution_evidence(self, events, required_tool):
        """Read execution facts from SDK observability events, not stream hints."""
        evidence = {"started": False, "finished": False, "succeeded": False,
                    "errors": [], "tool_call_ids": [], "started_ids": [],
                    "finished_ids": [], "error_ids": []}
        for event in events:
            if str(event.get("kind", "")).lower() != "profile":
                continue
            text = event.get("text")
            if not isinstance(text, str):
                continue
            try:
                payload, _ = self.c.parse_profile_event_text(text)
            except (TypeError, ValueError):
                continue
            if not isinstance(payload, dict):
                continue
            sdk_event = str(payload.get("sdk_event") or payload.get("event") or "")
            if sdk_event not in {"tool.start", "tool.finish", "tool.error"}:
                continue
            attrs = payload.get("attributes") if isinstance(payload.get("attributes"), dict) else {}
            if attrs.get("tool") != required_tool:
                continue
            call_id = attrs.get("tool_call_id")
            if isinstance(call_id, str) and call_id and call_id not in evidence["tool_call_ids"]:
                evidence["tool_call_ids"].append(call_id)
            if sdk_event == "tool.start":
                evidence["started"] = True
                if isinstance(call_id, str) and call_id and call_id not in evidence["started_ids"]:
                    evidence["started_ids"].append(call_id)
            elif sdk_event == "tool.error":
                evidence["recovered_after_error"] = False
                error = payload.get("error") or {}
                evidence["errors"].append(str(attrs.get("reason") or payload.get("message") or
                    error.get("message") or payload.get("status") or "tool error"))
                if isinstance(call_id, str) and call_id and call_id not in evidence["error_ids"]:
                    evidence["error_ids"].append(call_id)
            else:
                evidence["finished"] = True
                if isinstance(call_id, str) and call_id and call_id not in evidence["finished_ids"]:
                    evidence["finished_ids"].append(call_id)
                evidence["succeeded"] = evidence["succeeded"] or (
                    payload.get("status") == "ok" and attrs.get("ok") is True
                )
                if evidence["errors"] and payload.get("status") == "ok" and attrs.get("ok") is True and call_id in evidence["started_ids"]:
                    evidence["recovered_after_error"] = True
                if payload.get("status") == "error" or attrs.get("ok") is False:
                    error = payload.get("error") or {}
                    evidence["errors"].append(str(error.get("message") or attrs.get("reason") or "tool returned failure"))
                    evidence["recovered_after_error"] = False
                    if isinstance(call_id, str) and call_id and call_id not in evidence["error_ids"]:
                        evidence["error_ids"].append(call_id)
        if (evidence["started_ids"] and evidence["finished_ids"] and
                not set(evidence["finished_ids"]).issubset(set(evidence["started_ids"]))):
            evidence["errors"].append("tool execution result id did not match tool start")
            evidence["succeeded"] = False
            evidence["recovered_after_error"] = False
        return evidence

    def await_idle(self):
        self.tick(force=True)
        idle_deadline = time.monotonic() + self.a.conversation_timeout
        while not self.c.status_is_idle(self.c.flatten_status(self.status)) and time.monotonic() < idle_deadline:
            self.wait(.1)
        if not self.c.status_is_idle(self.c.flatten_status(self.status)):
            raise CaseFailure("device busy before test command", "precondition_busy")

    def model_case(self, prompt, required_tool=None, expected=None, reject_tools=False):
        self.await_idle()
        start, loss, errors = self.cursor, self.delivery_errors, len(self.runtime_errors)
        self.json_request("POST", "/api/messages", prompt)
        until = time.monotonic() + self.a.conversation_timeout
        events = []
        while time.monotonic() < until:
            self.wait(.1)
            events = [e for e in self.event_tail if self.c.event_seq(e) > start]
            # Drain the accepted run even after a transport error. Starting the
            # next case immediately would attribute the still-busy run to it.
            if self.delivery_errors != loss:
                raise RuntimeError("model event history incomplete")
            if any(self.c.is_completion_event(e) for e in events):
                self.tick(force=True)
                status = self.c.flatten_status(self.status)
                if self.c.status_is_idle(status):
                    if len(self.runtime_errors) == errors and not any(self.c.is_assistant_event(e) for e in events):
                        raise RuntimeError("model completed without answer text (possibly reasoning token limit)")
                    break
        else:
            if len(self.runtime_errors) > errors:
                raise CaseFailure(self.runtime_errors[-1], "transport_or_runtime_timeout",
                                  tool_execution=self._tool_execution_evidence(events, required_tool) if required_tool else None)
            raise RuntimeError("model run did not complete")
        answer = self.c.extract_response_text(events)
        tool_evidence = self._tool_execution_evidence(events, required_tool) if required_tool else None
        if len(self.runtime_errors) > errors:
            reason = self.runtime_errors[-1]
            category = "transport" if any(s in reason.lower() for s in ("http_client", "esp-tls", "transport", "chunked")) else "device_runtime"
            raise CaseFailure(reason, category, tool_execution=tool_evidence)
        tool_seen = required_tool is None or bool(tool_evidence and tool_evidence["started"])
        failure = None
        if reject_tools and any(
                e.get("kind") == "tool" and "tool start " in str(e.get("text", ""))
                for e in events):
            failure = "model invoked a tool although this case forbids tool calls"
        if required_tool and tool_evidence:
            if tool_evidence["errors"]:
                failure = f"{required_tool} execution failed: {tool_evidence['errors'][-1]}"
            elif not tool_evidence["started"]:
                failure = f"{required_tool} execution start event was missing"
            elif not tool_evidence["finished"] or not tool_evidence["succeeded"]:
                failure = f"{required_tool} execution did not finish successfully"
        ok = failure is None and tool_seen and (not expected or self.c.contains_all(answer, expected))
        if not ok and failure is None:
            failure = "required tool event or response marker was missing"
        self.logs["conversation"].write({"ts": self.c.utc_now(), "prompt": prompt, "answer": answer,
                                         "required_tool": required_tool, "tool_observed": tool_seen,
                                         "tool_execution": tool_evidence, "ok": ok,
                                         "failure": failure})
        if failure is not None:
            raise CaseFailure(failure, "tool_execution" if required_tool else "model_response",
                              tool_execution=tool_evidence)
        return {"response_excerpt": self.c.compact_text(answer), "tool_observed": tool_seen if required_tool else None,
                "tool_execution": tool_evidence,
                "evidence": "completed runtime + response + tool execution result; not physical output verification"}

    def functional_case(self, name, cycle):
        marker = f"CCT_{self.a.run_id.replace('-', '_')}_{cycle}_{name}"
        remote = self.cfg.get("workspace", {}).get("root", "/usb0/workspace").rstrip("/") + "/" + marker + ".lua"
        if name == "status":
            self.tick(force=True)
            if not self.status:
                raise RuntimeError("status telemetry unavailable")
            if self.status.get("usb", {}).get("mounted") is not True:
                raise RuntimeError("USB storage is not mounted")
            return {"heap": self.status.get("heap"), "wifi": redact(self.status.get("wifi")), "usb": self.status.get("usb")}
        if name == "events":
            return self.command("/heap", "internal_free=")
        if name == "profile":
            before = self.summary.profile_events
            self.json_request("POST", "/api/messages", "/profile sample")
            self.wait(max(2, self.a.sample_interval * 2))
            if self.summary.profile_events <= before:
                raise RuntimeError("profile sample missing from Web events")
            return {"samples": self.summary.profile_events, "valid_cpu_samples": self.summary.profile_cpu_valid_samples}
        if name == "config_audit":
            result = self.c.run_config_audit(self.http, self.summary)
            if result.get("risks"):
                raise RuntimeError("config audit risks: " + str(result["risks"]))
            return result
        if name == "ota_status":
            payload = json.loads(self.json_request("GET", "/api/ota/status"))
            if not isinstance(payload, dict) or not isinstance(payload.get("state"), str):
                raise RuntimeError("OTA status response is missing state")
            self.summary.observe_ota_status(payload)
            return {"state": payload.get("state"), "evidence": "status API only; no OTA upload or boot tested"}
        if name == "serial":
            if self.a.no_serial:
                return {"verdict": "skipped", "reason": "serial disabled"}
            return self.command(f'/lua print("{marker}")', marker, serial=True)
        if name == "lua":
            return self.command(f'/lua assert(6*7==42); print("{marker}")', marker)
        if name in {"file_io", "lua_file"}:
            if name == "lua_file":
                remote = self.cfg.get("workspace", {}).get("root", "/usb0/workspace").rstrip("/") + "/lua/" + marker + ".lua"
            content = f'print("{marker}")\n'
            self.json_request("PUT", "/api/file", content, {"path": remote})
            readback = self.json_request("GET", "/api/file", query={"path": remote})
            if readback != content:
                raise RuntimeError("USB file readback mismatch")
            result = self.command("/luafile " + marker + ".lua", marker) if name == "lua_file" else {"evidence": "exact file readback"}
            return {**result, "test_file": remote, "retained": True}
        if name == "display":
            result = self.command(f'/lua screen.append("{marker}"); screen.flush(); print("{marker}")', marker)
            return {**result, "physical_display": "manual verification required"}
        if name == "llm":
            return self.model_case(f"只回复 {marker}，不要调用工具。", expected=[marker], reject_tools=True)
        if name == "context":
            self.model_case(f"记住本次测试口令 {marker}，回复已记住；不要写入长期记忆。", reject_tools=True)
            return self.model_case("请只回复刚才让你记住的测试口令。", expected=[marker], reject_tools=True)
        if name in {"camera", "memory"}:
            tool = "camera_capture" if name == "camera" else "memory"
            enabled = self.cfg.get("tools", {}).get("enabled")
            if isinstance(enabled, list) and tool not in enabled:
                return {"verdict": "skipped", "reason": f"{tool} disabled in device config"}
            prompt = (f"调用 camera_capture 拍一张小图，save_path 必须为 {marker}.jpg，return_image=true，然后简短描述，不要重复拍摄。" if name == "camera" else
                      "调用 memory 工具只读检索测试相关记忆，不要创建、更新或删除记忆，然后简短回复。")
            result = self.model_case(prompt, required_tool=tool)
            if name == "camera":
                path = self.cfg.get("workspace", {}).get("root", "/usb0/workspace").rstrip("/") + "/" + marker + ".jpg"
                download_errors = []
                for attempt in range(2):
                    self.check()
                    try:
                        code, image, elapsed = self.http.request_bytes("GET", "/api/file", query={"path": path}, max_bytes=1024 * 1024)
                        self.summary.observe_http_latency("/api/file", elapsed)
                        if not 200 <= code < 300:
                            raise CaseFailure(f"camera image download returned HTTP {code}", "camera_output", **result, test_file=path)
                        self.summary.http_ok += 1
                        break
                    except (OSError, http.client.HTTPException) as exc:
                        download_errors.append(str(exc))
                        self.summary.fail(str(exc), http=True)
                        if attempt:
                            raise CaseFailure(str(exc), "artifact_transport", **result, test_file=path,
                                              transport_category=self.c.classify_http_error(str(exc)), download_errors=download_errors) from exc
                        self.http.close()
                    except ValueError as exc:
                        raise CaseFailure(str(exc), "camera_output", **result, test_file=path) from exc
                if len(image) > 1024 * 1024 or len(image) < 4 or not image.startswith(b"\xff\xd8") or not image.endswith(b"\xff\xd9"):
                    raise CaseFailure("camera output is missing or not a bounded JPEG file", "camera_output", **result, test_file=path)
                return {**result, "test_file": path, "jpeg_bytes": len(image), "sha256": hashlib.sha256(image).hexdigest(),
                        "download_errors": download_errors,
                        "physical_image": "manual verification required"}
            return {**result, "verdict": "pass", "reason": "tool execution and tool.finish observability verified"}
        raise ValueError("unknown functional case")

    def run_functional(self):
        cycle = 0
        while True:
            self.check()
            cycle += 1
            for name in self.a.functional_cases_list:
                self.phase = f"functional:{cycle}:{name}"
                started = time.monotonic()
                row = {"ts": self.c.utc_now(), "cycle": cycle, "case": name, "verdict": "pass"}
                try:
                    row.update(self.functional_case(name, cycle))
                except StopRun:
                    row.update(verdict="inconclusive", reason="test stopped before this case completed",
                               failure_category="test_interrupted", stop_reason=self.summary.stop_reason)
                    self.cases.append(row)
                    self.logs["functional_cases"].write(row)
                    raise
                except CaseFailure as exc:
                    row.update(verdict="inconclusive" if exc.category in {"precondition_busy", "artifact_transport"} else "fail",
                               reason=str(exc), failure_category=exc.category, **exc.evidence)
                except OSError as exc:
                    row.update(verdict="inconclusive", reason=str(exc), failure_category="host_io_or_transport",
                               transport_category=self.c.classify_http_error(str(exc)))
                except Exception as exc:
                    row.update(verdict="fail", reason=str(exc))
                row["elapsed_ms"] = round((time.monotonic() - started) * 1000, 2)
                self.cases.append(row)
                self.logs["functional_cases"].write(row)
                self.save()
                print(f"cycle={cycle} case={name} verdict={row['verdict']}", flush=True)
                if row["verdict"] == "fail":
                    self.recover(self.a.recovery_seconds)
                self.wait(self.a.functional_interval)
            self.recover(self.a.recovery_seconds)

    def load_stage(self, stage, confirmation=False, warmup_repeat=False):
        self.check()
        suffix = "repeat_after_warmup" if warmup_repeat else "confirm" if confirmation else "probe"
        stage_id = f"{stage['axis']}_{stage['level']}_{suffix}"
        self.phase = stage_id
        before = dict(self.status.get("heap", {}))
        samples_before = len(self.resource_samples)
        history_before = event_history_snapshot(self.c.flatten_status(self.status))
        self.last_recovery = {}
        counts_before = (self.delivery_errors, self.summary.reboots_observed + self.summary.event_cursor_resets,
                         len(self.runtime_errors), self.health_failures, self.telemetry_errors)
        out: queue.Queue = queue.Queue(maxsize=8192)
        halt = threading.Event()
        began = time.monotonic()
        until = min(self.deadline, began + self.a.stage_seconds)
        records = []
        row = {"ts": self.c.utc_now(), "stage_id": stage_id, "load": stage, "confirmation": confirmation,
               "warmup_repeat": warmup_repeat,
               "connection_plan": {"load_clients": stage["clients"], "observer_clients": 1,
                                   "total_planned": stage["clients"] + 1, "external_clients": "not_counted"}}
        self.completions.clear()

        def worker(index):
            client = self.http.fork()
            number = 0
            due = began + index / max(1, stage["clients"]) / stage["per_client_rps"]
            try:
                while not halt.is_set() and time.monotonic() < until:
                    if halt.wait(max(0, due - time.monotonic())):
                        break
                    if time.monotonic() >= until:
                        break
                    number += 1
                    marker = f"CCT_{stage_id}_{index}_{number}"
                    sent = time.monotonic()
                    item = {"started": sent, "marker": marker if stage["axis"] != "http" else None,
                            "client": index, "http_status": 0}
                    try:
                        if stage["axis"] == "http":
                            path = "/api/status" if number % 2 else "/api/events"
                            code, text, _ = client.request_text("GET", path, query={"since": 0, "limit": 1} if path.endswith("events") else None)
                            expected = "status" if path.endswith("status") else "events"
                        else:
                            code, text, _ = client.request_text("POST", "/api/messages", body=lua_workload(stage["axis"], stage["value"], marker))
                            expected = "command"
                        item.update(pressure_response(code, text, expected))
                    except Exception as exc:
                        item.update(ok=False, error=str(exc), error_category="invalid_response" if item["http_status"] else self.c.classify_http_error(str(exc)))
                    item["timing"] = dict(client.last_timing)
                    item["elapsed_ms"] = round((time.monotonic() - sent) * 1000, 3)
                    # Backpressure is accounted for; the producer never grows an unbounded queue.
                    while True:
                        try:
                            out.put(item, timeout=.1)
                            break
                        except queue.Full:
                            if halt.is_set():
                                return
                    due = max(due + 1 / stage["per_client_rps"], time.monotonic())
            finally:
                client.close()

        def drain():
            while True:
                try:
                    record = out.get_nowait()
                except queue.Empty:
                    break
                records.append(record)
                self.logs["pressure_requests"].write({"ts": self.c.utc_now(), "stage_id": stage_id, **record})

        threads = [threading.Thread(target=worker, args=(i,), daemon=True) for i in range(stage["clients"])]
        interrupted = False
        recovered = False
        try:
            for thread in threads:
                thread.start()
            while time.monotonic() < until:
                drain()
                self.tick()
                if self.summary.reboots_observed + self.summary.event_cursor_resets > counts_before[1]:
                    self.summary.stop_reason = "device_reboot"
                    raise StopRun()
                self.stop.wait(.1)
        except StopRun:
            interrupted = True
        finally:
            halt.set()
            join_deadline = time.monotonic() + self.a.http_timeout + 1
            while any(t.is_alive() for t in threads) and time.monotonic() < join_deadline:
                drain()
                for thread in threads:
                    thread.join(.01)
            drain()
            row["workers_still_running"] = sum(t.is_alive() for t in threads)
            if row["workers_still_running"]:
                interrupted = True
                self.summary.stop_reason = "load_workers_not_stopped"
        exposure = min(time.monotonic(), until) - began
        camera_report = camera_processing_report([r for r in self.resource_samples[samples_before:] if r.get("phase") == stage_id])
        if not interrupted:
            try:
                recovered = self.recover(self.a.recovery_seconds)
            except StopRun:
                interrupted = True
        heap_ok, deltas = heap_recovered(before, self.status.get("heap", {}), self.a.heap_drop_bytes, self.a.heap_drop_ratio)
        metrics = summarize_requests(records, self.completions, exposure, stage["clients"] * stage["per_client_rps"])
        observation_loss = (self.delivery_errors > counts_before[0] or self.telemetry_errors > counts_before[4]
                            or self.health_failures > counts_before[3])
        runtime_errors = self.runtime_errors[counts_before[2]:]
        rebooted = self.summary.reboots_observed + self.summary.event_cursor_resets > counts_before[1]
        history_after = event_history_snapshot(self.c.flatten_status(self.status))
        warmup_candidate = (not interrupted and not confirmation and not warmup_repeat and
                            event_history_warmup_candidate(history_before, history_after, deltas, metrics,
                                                           recovered, not observation_loss and not runtime_errors and not rebooted))
        recovery_ok = recovered and heap_ok
        evaluation = evaluate_stage(metrics, p95_limit=self.a.p95_limit_ms, error_limit=self.a.error_rate_limit,
                                    delivery_loss=observation_loss, runtime_errors=runtime_errors, rebooted=rebooted,
                                    recovered=recovery_ok,
                                    complete=not interrupted and not row["workers_still_running"])
        warmup_candidate = (warmup_candidate and "recovery_failed" in evaluation["reasons"] and
                            set(evaluation["reasons"]) <= {"recovery_failed", "dns_or_mdns_errors"})
        row.update(evaluation, metrics=metrics, heap_recovery=deltas, recovered=recovery_ok,
                   camera_processing=camera_report,
                   recovery={**self.last_recovery, "heap_within_threshold": heap_ok, "recovered": recovery_ok},
                   event_history={"before": history_before, "after": history_after}, warmup_candidate=warmup_candidate,
                   runtime_errors=runtime_errors[-10:], exposure_seconds=exposure)
        if warmup_candidate:
            row.update(verdict="inconclusive", limit_kind=None,
                       observation_issue=evaluation["observation_issue"] or "event_history_warmup")
            row["reasons"].append("event_history_fill_requires_repeat")
        if interrupted:
            hard_stop = self.summary.stop_reason in {"device_reboot", "internal_heap_floor"}
            row["verdict"] = "limit" if hard_stop else "inconclusive"
            if hard_stop:
                row["reasons"].append(self.summary.stop_reason)
            row["stop_reason"] = self.summary.stop_reason
        self.logs["pressure_stages"].write(row)
        print(f"stage={stage_id} verdict={row['verdict']} rps={metrics['achieved_rps']:.2f} reasons={row['reasons']}", flush=True)
        if interrupted:
            self.boundaries.setdefault(stage["axis"], {})["interrupted_stage"] = row
            if row["verdict"] == "limit":
                self.boundaries[stage["axis"]].update(first_limit=row, boundary_found=True)
            raise StopRun()
        return row

    def run_stress(self):
        plan = stage_plan(self.a.stress_axes_list, self.a.stress_clients_list, self.a.stress_rate)
        for axis in self.a.stress_axes_list:
            boundary = {"last_passing": None, "first_limit": None, "inconclusive_stage": None, "confirmed": False, "boundary_found": False}
            self.boundaries[axis] = boundary
            for stage in [s for s in plan if s["axis"] == axis]:
                row = self.load_stage(stage)
                if row.get("warmup_candidate"):
                    boundary["warmup_stage"] = row
                    boundary["warmup_observation_issue"] = row.get("observation_issue") if row.get("observation_issue") != "event_history_warmup" else None
                    row = self.load_stage(stage, warmup_repeat=True)
                    boundary["warmup_repeat"] = row
                if row["verdict"] == "pass":
                    boundary["last_passing"] = row
                elif row["verdict"] == "limit":
                    boundary["first_limit"] = row
                    boundary["boundary_found"] = True
                    break
                else:
                    boundary["inconclusive_stage"] = row
                    break
                self.save()
            if boundary["first_limit"] and not boundary["first_limit"]["recovered"]:
                self.summary.stop_reason = recovery_failure_reason(boundary["first_limit"])
                boundary["interpretation"] = self.summary.stop_reason
                raise StopRun()
            if boundary["inconclusive_stage"] and not boundary["inconclusive_stage"]["recovered"]:
                self.summary.stop_reason = recovery_failure_reason(boundary["inconclusive_stage"], "recovery_unobserved")
                boundary["interpretation"] = self.summary.stop_reason
                raise StopRun()
            if boundary["last_passing"]:
                confirmed = self.load_stage(boundary["last_passing"]["load"], confirmation=True)
                boundary["confirmation"] = confirmed
                boundary["confirmed"] = confirmed["verdict"] == "pass"
                if not confirmed["recovered"]:
                    boundary["interpretation"] = "confirmation_recovery_failed"
                    self.summary.stop_reason = recovery_failure_reason(confirmed)
                    raise StopRun()
            if boundary["inconclusive_stage"]:
                boundary["interpretation"] = "load_or_observation_inconclusive"
            elif boundary["last_passing"] and not boundary["confirmed"]:
                boundary["interpretation"] = "confirmation_failed"
            elif boundary["boundary_found"]:
                boundary["interpretation"] = "bracket_observed" if boundary["last_passing"] else "upper_bound_only_no_passing_level"
            else:
                boundary["interpretation"] = "lower_bound_only_limit_not_reached"
            self.save()
        incomplete_search = any(b.get("warmup_observation_issue") or b.get("interpretation") in {"load_or_observation_inconclusive", "confirmation_failed"}
                                for b in self.boundaries.values())
        self.summary.stop_reason = "boundary_search_inconclusive" if incomplete_search else "boundary_search_complete"

    def resource_report(self):
        data = {}
        for key in ("internal_free", "internal_largest", "psram_free", "psram_largest"):
            values = [(r["elapsed"], r["heap"].get(key)) for r in self.resource_samples if isinstance(r["heap"].get(key), (float, int))]
            if not values:
                continue
            xs, ys = zip(*values)
            avg_x, avg_y = statistics.mean(xs), statistics.mean(ys)
            denominator = sum((x - avg_x) ** 2 for x in xs)
            slope = sum((x - avg_x) * (y - avg_y) for x, y in values) / denominator if denominator else None
            data[key] = {"min": min(ys), "max": max(ys), "first": ys[0], "last": ys[-1],
                         "slope_bytes_per_minute": slope * 60 if slope is not None else None}
        stable, drops = heap_recovered(self.idle_samples[0]["heap"], self.idle_samples[-1]["heap"], self.a.heap_drop_bytes, self.a.heap_drop_ratio) if len(self.idle_samples) >= 3 else (False, {})
        return {"samples": len(self.resource_samples), "metrics": data, "idle_samples": self.idle_samples,
                "idle_recovery": drops, "stability": "insufficient_idle_samples" if len(self.idle_samples) < 3 else "within_threshold" if stable else "retained_heap_drop",
                "warmup_followups": {axis: {"initial_heap_recovery": b["warmup_stage"].get("heap_recovery"),
                                             "original_observation_issue": b.get("warmup_observation_issue"),
                                             "repeat_heap_recovery": b.get("warmup_repeat", {}).get("heap_recovery"),
                                             "repeat_verdict": b.get("warmup_repeat", {}).get("verdict", "not_completed")}
                                      for axis, b in self.boundaries.items() if b.get("warmup_stage")},
                "diagnosis": "insufficient_idle_samples" if len(self.idle_samples) < 3 else "retention_or_fragmentation_requires_followup" if not stable else "no_threshold_breach_observed",
                "note": "Trend includes active load; idle recovery is a regression indicator, not proof of absence of leaks."}

    def save(self, final=False):
        now = time.monotonic()
        if not final and now < self.next_save:
            return
        self.next_save = now + 1.0
        self.report_sequence += 1
        self.summary.elapsed_seconds = round(time.monotonic() - self.started, 3)
        if final:
            self.summary.finished_at = self.c.utc_now()
        payload = self.summary.as_dict()
        payload.update(mode=self.a.mode, verdict=self.result, functional_cases=self.cases,
                       dns_policy="session_cache_refresh_on_connect_failure" if self.a.mode == "stress" else "60_second_cache",
                       boundaries=self.boundaries, resource_stability=self.resource_report(), manual_coverage=MANUAL_COVERAGE,
                       report_write_errors=self.report_write_errors, last_report_error=self.last_report_error,
                       report_sequence=self.report_sequence)
        # Publish a complete document to the dashboard reader.
        temporary = self.output / "summary.json.tmp"
        serialized = json.dumps(payload, ensure_ascii=False, indent=2)
        try:
            temporary.write_text(serialized, encoding="utf-8")
            for attempt in range(4):
                try:
                    temporary.replace(self.output / "summary.json")
                    return
                except PermissionError:
                    if attempt == 3:
                        raise
                    time.sleep(.01 * (attempt + 1))
        except OSError as exc:
            self.report_write_errors += 1
            self.last_report_error = str(exc)
            # Report publication must not abort board observation or become a
            # camera/LLM failure. Retain the old valid snapshot and retry later.
            if self.report_write_errors == 1 or final:
                print(f"Report publication failed: {exc}", flush=True)
            if final:
                payload.update(report_write_errors=self.report_write_errors,
                               last_report_error=self.last_report_error)
                try:
                    (self.output / "summary.final.json").write_text(
                        json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
                except OSError as fallback_error:
                    print(f"Final report fallback failed: {fallback_error}", flush=True)

    def finish(self):
        # Drain after the reader is stopped, including warnings during shutdown.
        while True:
            lines = self.serial.recent_lines(10000)
            if not lines:
                break
            for line in lines:
                self.summary.observe_serial_line(line)
        if self.a.mode == "functional":
            failed = any(row["verdict"] == "fail" for row in self.cases)
            unresolved = any(row["verdict"] == "inconclusive" and not (
                row is self.cases[-1] and self.summary.stop_reason == "duration_complete" and
                row.get("failure_category") == "test_interrupted" and row.get("stop_reason") == "duration_complete"
            ) for row in self.cases)
            required = set(self.a.functional_cases_list) - ({"serial"} if self.a.no_serial else set())
            incomplete = required - {r["case"] for r in self.cases if r["verdict"] == "pass"}
            resources = self.resource_report()
            cpu_missing = "profile" in self.a.functional_cases_list and self.summary.profile_cpu_valid_samples == 0
            self.result = "fail" if failed or resources["stability"] == "retained_heap_drop" or self.summary.reboots_observed or self.summary.serial_errors else (
                "inconclusive" if unresolved or incomplete or cpu_missing or resources["stability"] != "within_threshold" or self.delivery_errors or self.telemetry_errors or self.health_failures else "pass")
        else:
            uncertain = any(b.get("warmup_observation_issue") or b.get("interpretation") in {"load_or_observation_inconclusive", "confirmation_failed"} for b in self.boundaries.values())
            self.result = "boundary_search_complete" if self.summary.stop_reason == "boundary_search_complete" and not uncertain else "inconclusive"
        if self.summary.stop_reason in {"user_stop", "signal", "exception", "device_unreachable", "internal_heap_floor", "device_reboot", "recovery_failed", "heap_recovery_failed", "recovery_unobserved", "load_workers_not_stopped"}:
            self.result = "inconclusive" if self.result != "fail" else self.result
        if self.summary.task_watchdog_warnings:
            self.result = "fail"
        self.save(final=True)
        lines = [f"C-Claw {self.a.mode} test", f"Result: {self.result}", f"Stop: {self.summary.stop_reason}",
                 f"Elapsed: {self.summary.elapsed_seconds}s",
                 f"Task watchdog warnings: {self.summary.task_watchdog_warnings}",
                 f"Report publication failures: {self.report_write_errors}; last: {self.last_report_error}", "", "Functional cases:"]
        lines.extend(f"  cycle={r['cycle']} {r['case']}: {r['verdict']} {r.get('reason', '')}" for r in self.cases)
        lines.append("\nPressure boundary brackets (configuration and test duration dependent):")
        for axis, b in self.boundaries.items():
            good, bad = b.get("last_passing"), b.get("first_limit")
            lines.append(f"  {axis}: passing={good['load'] if good else None}; confirmed={b.get('confirmed')}; first_limit={bad['load'] if bad else None}; {b.get('interpretation', 'incomplete')}")
            if bad:
                lines.append(f"    kind={bad.get('limit_kind')} reasons={bad.get('reasons')}")
            if b.get("inconclusive_stage"):
                row = b["inconclusive_stage"]
                lines.append(f"    inconclusive={row['load']} issue={row.get('observation_issue')} reasons={row.get('reasons')}")
            if b.get("warmup_stage"):
                lines.append(f"    event history warmup retained; repeat verdict={b.get('warmup_repeat', {}).get('verdict', 'not_completed')}")
                if b.get("warmup_observation_issue"):
                    lines.append(f"    original observation interference retained: {b['warmup_observation_issue']}")
        lines.extend(["\nResources:", json.dumps(self.resource_report(), ensure_ascii=False, indent=2),
                      f"CPU valid samples: {self.summary.profile_cpu_valid_samples}; CPU maximum: {self.summary.profile_cpu_total_max_pct if self.summary.profile_cpu_valid_samples else 'unobserved'}",
                      "\nManual / not executed:", *MANUAL_COVERAGE,
                      "\nNo board-wide maximum is inferred from cloud model throughput or firmware sandbox guards."])
        report = "\n".join(lines) + "\n"
        try:
            (self.output / "summary_report.txt").write_text(report, encoding="utf-8")
        except OSError as exc:
            print(f"Text report publication failed: {exc}", flush=True)
        print(report, flush=True)

    def run(self):
        try:
            for sig in (signal.SIGINT, signal.SIGTERM):
                self.old_signals[sig] = signal.signal(sig, self.signal_stop)
            self.serial.start()
            baseline = self.startup_read("/api/events", query={"since": 0, "limit": 1})
            self.cursor = self.c.event_cursor(baseline, 0)
            self.cfg = self.startup_read("/api/config")
            (self.output / "device_config.redacted.json").write_text(json.dumps(redact(self.cfg), ensure_ascii=False, indent=2), encoding="utf-8")
            self.c.apply_profile_guard(self.http, self.summary, self.a)
            self.phase = "baseline"
            self.wait(self.a.baseline_seconds)
            if not self.recover(self.a.recovery_seconds):
                self.summary.stop_reason = "baseline_not_idle"
                raise StopRun()
            if self.a.mode == "functional":
                self.run_functional()
            else:
                self.run_stress()
        except StopRun:
            pass
        except Exception as exc:
            self.summary.stop_reason = "exception"
            self.summary.last_error = str(exc)
            print(f"test error: {exc}", flush=True)
        finally:
            self.serial.close()
            self.http.close()
            for sig, handler in self.old_signals.items():
                signal.signal(sig, handler)
            self.finish()
            for log in self.logs.values():
                log.close()
        return 0 if self.result in {"pass", "boundary_search_complete"} else 1


def add_arguments(parser):
    parser.add_argument("--mode", choices=["functional", "stress"], default="functional")
    parser.add_argument("--functional-cases", default=",".join(FUNCTIONAL_CASES))
    parser.add_argument("--functional-interval", type=float, default=2)
    parser.add_argument("--sample-interval", type=float, default=2)
    parser.add_argument("--baseline-seconds", type=float, default=10)
    parser.add_argument("--recovery-seconds", type=float, default=20)
    parser.add_argument("--stress-axes", default=",".join(STRESS_AXES))
    parser.add_argument("--stress-clients", default="1,2,4,8,16")
    parser.add_argument("--stress-rate", type=float, default=2, help="Requests per second per load client")
    parser.add_argument("--stage-seconds", type=float, default=120)
    parser.add_argument("--p95-limit-ms", type=float, default=2000)
    parser.add_argument("--error-rate-limit", type=float, default=.01)
    parser.add_argument("--heap-drop-bytes", type=int, default=32768)
    parser.add_argument("--heap-drop-ratio", type=float, default=.1)
    parser.add_argument("--min-internal-free", type=int, default=16384)
    parser.add_argument("--unhealthy-limit", type=int, default=5)


def validate_arguments(args):
    args.functional_cases_list = [s.strip() for s in args.functional_cases.split(",") if s.strip()]
    args.stress_axes_list = [s.strip() for s in args.stress_axes.split(",") if s.strip()]
    args.stress_clients_list = [int(s) for s in args.stress_clients.split(",")]
    if not args.functional_cases_list or set(args.functional_cases_list) - set(FUNCTIONAL_CASES):
        raise ValueError("unknown or empty functional cases")
    if not args.stress_axes_list or len(set(args.stress_axes_list)) != len(args.stress_axes_list) or set(args.stress_axes_list) - set(STRESS_AXES):
        raise ValueError("unknown or empty stress axes")
    if not args.stress_clients_list or args.stress_clients_list != sorted(set(args.stress_clients_list)) or not all(1 <= n <= 64 for n in args.stress_clients_list):
        raise ValueError("stress clients must be ascending unique values in 1..64")
    for key in ("functional_interval", "sample_interval", "baseline_seconds", "recovery_seconds", "stress_rate", "stage_seconds", "p95_limit_ms"):
        if not math.isfinite(getattr(args, key)) or getattr(args, key) <= 0:
            raise ValueError(f"{key} must be finite and positive")
    if not 0 <= args.error_rate_limit < 1 or not 0 <= args.heap_drop_ratio < 1:
        raise ValueError("error/heap ratios must be in [0,1)")
    if args.heap_drop_bytes < 0 or args.min_internal_free < 0 or args.unhealthy_limit < 1:
        raise ValueError("invalid recovery or health guard")
    if args.append:
        raise ValueError("two-mode tests require a fresh run directory; --append is not supported")


def run_mode(args, common):
    if args.dry_run:
        output = common.resolve_output_dir(Path(args.output_dir), False, args.run_id)
        output.mkdir(parents=True, exist_ok=True)
        summary = common.init_summary(args, output)
        summary.stop_reason = "dry_run"
        payload = summary.as_dict()
        plan = stage_plan(args.stress_axes_list, args.stress_clients_list, args.stress_rate)
        payload.update(mode=args.mode, verdict="not_run", functional_cases=args.functional_cases_list,
                       pressure_plan=plan if args.mode == "stress" else [], manual_coverage=MANUAL_COVERAGE)
        payload["finished_at"] = common.utc_now()
        (output / "summary.json").write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
        (output / "summary_report.txt").write_text(f"Mode: {args.mode}\nDry run only; no hardware tested.\n" + json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
        print(f"dry-run: mode={args.mode}, output={output}")
        return 0
    return Session(args, common).run()
