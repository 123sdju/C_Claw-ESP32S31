"""Simulated board tests: no network, serial port, compiler or firmware required."""
import contextlib
import io
import json
import os
import re
import threading
import time
import unittest
from pathlib import Path
from unittest.mock import patch

import firmware_test_modes as modes
import long_run_test as common
import long_run_dashboard as dashboard


class Board:
    def __init__(self, completion=True, reject=False):
        self.lock = threading.Lock()
        self.events = []
        self.files = {}
        self.completion, self.reject = completion, reject
        self.active = self.max_active = 0
        self.status_reads = self.messages = 0
        self.request_barrier = None

    def handle(self, method, path, body, query):
        with self.lock:
            self.active += 1
            self.max_active = max(self.max_active, self.active)
            barrier = self.request_barrier if path == "/api/messages" else None
        try:
            if barrier is not None:
                barrier.wait(timeout=2)
            time.sleep(.003)
            with self.lock:
                if path == "/api/status":
                    self.status_reads += 1
                    result = {"uptime_ms": int(time.monotonic() * 1000),
                              "heap": {"internal_free": 200000, "internal_largest": 100000,
                                       "psram_free": 1000000, "psram_largest": 900000},
                              "usb": {"mounted": True}, "interaction": {"busy": False, "pending": 0}}
                elif path == "/api/events":
                    result = {"latest_seq": len(self.events), "events": [e for e in self.events if e["seq"] > query.get("since", 0)][-query.get("limit", 1000):]}
                elif path == "/api/config":
                    result = {"workspace": {"root": "/usb0/workspace"}, "model": {"api_key": "***"}}
                elif path == "/api/messages":
                    self.messages += 1
                    if self.reject:
                        return 400, json.dumps({"error": "ESP_ERR_TIMEOUT"})
                    if self.completion:
                        markers = re.findall(r"CCT_[A-Za-z0-9_]+", body)
                        text = markers[-1] if markers else "internal_free=200000"
                        if body.startswith("/luafile "):
                            text = re.findall(r"CCT_[A-Za-z0-9_]+", self.files.get("/usb0/workspace/lua/" + body[9:], ""))[-1]
                        self.events.append({"seq": len(self.events) + 1, "kind": "tool", "level": "info", "text": text})
                    result = {"ok": True, "queued": True}
                elif path == "/api/file":
                    if method == "PUT":
                        self.files[query["path"]] = body
                        result = {"ok": True}
                    else:
                        return 200, self.files[query["path"]]
                else:
                    result = {"state": "idle"}
                return 200, json.dumps(result)
        finally:
            with self.lock:
                self.active -= 1

    def client_class(self):
        board = self
        class Client:
            def __init__(self, *args, **kwargs):
                self.last_timing = {}
                self.resolver = common.AddressResolver()
            def request_text(self, method, path, body=None, query=None):
                start = time.monotonic()
                status, text = board.handle(method, path, body, query or {})
                return status, text, time.monotonic() - start
            def close(self):
                pass
            def fork(self):
                return Client()
        return Client


def scratch():
    folder = Path(__file__).resolve().parents[1] / "tmp" / "firmware-mode-tests" / str(time.time_ns())
    folder.mkdir(parents=True)
    return folder


def args_for(folder, extra=()):
    args = common.build_parser().parse_args(["--output-dir", str(folder), "--no-serial", "--no-profile-guard",
        "--duration", "20s", "--functional-cases", "status,lua,file_io,lua_file", "--sample-interval", ".02",
        "--event-poll-interval", ".02", "--baseline-seconds", ".02", "--recovery-seconds", ".05",
        "--functional-interval", ".01", "--stage-seconds", ".3", "--stress-rate", "30", *extra])
    args.run_id = "simulated"
    args.duration_seconds = common.parse_duration(args.duration)
    args.test_items_set = set()
    modes.validate_arguments(args)
    return args


def metric(ok=True):
    return {"requests": 10, "error_rate": 0 if ok else .2, "http_p95_ms": 10,
            "completion_ratio": 1, "error_examples": [], "offered_rate_reached": True}


def evaluate(m, **kwargs):
    defaults = dict(p95_limit=2000, error_limit=.01, delivery_loss=False, runtime_errors=[],
                    rebooted=False, recovered=True, complete=True)
    defaults.update(kwargs)
    return modes.evaluate_stage(m, **defaults)


class ModeTests(unittest.TestCase):
    def test_watchdog_overrides_lua_guard_and_completed_boundary(self):
        result = evaluate(metric(), runtime_errors=["task_watchdog: triggered", "Lua execution timed out after 1000 ms"])
        self.assertEqual(result["limit_kind"], "firmware_task_watchdog")
        session = modes.Session(args_for(scratch(), ["--mode", "stress"]), common)
        try:
            session.summary.stop_reason = "boundary_search_complete"
            session.serial.lines.put("E (1000) task_wdt: Task watchdog got triggered. The following tasks/users did not reset the watchdog in time:")
            session.serial.lines.put("E (1000) task_wdt:  - IDLE0 (CPU 0)")
            with contextlib.redirect_stdout(io.StringIO()):
                session.finish()
            self.assertEqual(session.summary.task_watchdog_warnings, 1)
            self.assertEqual(session.result, "fail")
            report = json.loads((session.output / "summary.json").read_text(encoding="utf-8"))
            self.assertEqual(report["task_watchdog_warnings"], 1)
            self.assertEqual(report["verdict"], "fail")
        finally:
            for log in session.logs.values():
                log.close()

    def test_default_mode_is_functional_and_alias_selects_stress(self):
        for extra, expected in [([], "functional"), (["--mode", "stress"], "stress"), (["--stress"], "stress")]:
            with patch.object(modes, "run_mode", return_value=0) as run:
                common.main(["--dry-run", *extra])
            self.assertEqual(run.call_args.args[0].mode, expected)
        with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
            common.main(["--stress", "--mode", "functional"])

    def test_plan_is_increasing_and_separates_resource_axes(self):
        plan = modes.stage_plan(modes.STRESS_AXES, [1, 2, 4], 2)
        self.assertEqual([s["clients"] for s in plan if s["axis"] == "http"], [1, 2, 4])
        self.assertTrue(all(s["clients"] == 1 for s in plan if s["axis"] == "lua_memory"))
        self.assertIn("10000", modes.lua_workload("lua_cpu", 10000, "CCT_cpu"))

    def test_acceptance_is_not_completion(self):
        requests = [{"ok": True, "started": 1, "marker": "CCT_1", "elapsed_ms": 10, "http_status": 200}]
        metrics = modes.summarize_requests(requests, {}, 1, 1)
        self.assertEqual(metrics["accepted"], 1)
        self.assertEqual(metrics["commands_completed"], 0)
        self.assertIn("accepted_commands_not_completed", evaluate(metrics)["reasons"])

    def test_guard_rejection_loss_reboot_and_under_driven_load(self):
        self.assertEqual(evaluate(metric(), runtime_errors=["Lua execution timed out after 1000 ms"])["limit_kind"], "firmware_lua_guard")
        m = metric(False)
        m["error_examples"] = ["ESP_ERR_TIMEOUT"]
        self.assertEqual(evaluate(m)["limit_kind"], "firmware_queue_rejection")
        self.assertEqual(evaluate(metric(), delivery_loss=True)["verdict"], "inconclusive")
        self.assertEqual(evaluate(metric(), rebooted=True)["verdict"], "limit")
        m = metric()
        m["offered_rate_reached"] = False
        self.assertEqual(evaluate(m)["verdict"], "inconclusive")

    def test_dns_failures_cannot_establish_a_board_boundary(self):
        rows = [{"ok": False, "http_status": 0, "error": "[Errno 11001] getaddrinfo failed",
                 "error_category": "dns_or_mdns", "elapsed_ms": 5432} for _ in range(35)]
        metrics = modes.summarize_requests(rows, {}, 120, 2)
        result = evaluate(metrics)
        self.assertEqual(metrics["dns_errors"], 35)
        self.assertEqual(metrics["transport_errors"], 0)
        self.assertIsNone(metrics["http_p95_ms"])
        self.assertEqual(result["verdict"], "inconclusive")
        self.assertIsNone(result["limit_kind"])
        self.assertEqual(result["observation_issue"], "dns_or_mdns")

    def test_plain_text_http_queue_rejection_is_not_a_json_error(self):
        row = modes.pressure_response(400, "ESP_ERR_TIMEOUT", "command")
        self.assertEqual(row["http_status"], 400)
        self.assertEqual(row["error_category"], "http_response")
        self.assertIn("ESP_ERR_TIMEOUT", row["error"])
        metrics = modes.summarize_requests([{**row, "elapsed_ms": 20} for _ in range(10)], {}, 5, 2)
        self.assertEqual(evaluate(metrics)["limit_kind"], "firmware_queue_rejection")
        self.assertEqual(modes.pressure_response(200, "not JSON", "command")["error_category"], "invalid_response")

    def test_camera_rate_reports_processing_only_and_rejects_counter_reset(self):
        samples = [{"elapsed": 0, "status": {"camera_frames": 100}},
                   {"elapsed": 10, "status": {"camera_frames": 170}}]
        report = modes.camera_processing_report(samples)
        self.assertEqual(report["processed_fps"], 7)
        self.assertIn("do not verify LCD colors", report["note"])
        samples[-1]["status"]["camera_frames"] = 10
        self.assertIsNone(modes.camera_processing_report(samples)["processed_fps"])

    def test_ui_preview_handoff_counters_are_separate_from_camera_frames(self):
        samples = []
        for elapsed, frames, updates, replaced in ((0, 100, 20, 0), (10, 340, 170, 40)):
            samples.append({"elapsed": elapsed, "status": common.flatten_status({"camera": {
                "frames": frames, "ui_preview": {"source_updates": updates, "replaced_pending_frames": replaced,
                "cache_sync_errors": 0, "input_row_bytes": 480, "stride_bytes": 512, "buffer_bytes": 122880, "buffer_count": 5}}})})
        report = modes.camera_processing_report(samples)
        self.assertEqual(report["processed_fps"], 24)
        self.assertEqual(report["ui_source_updates_per_second"], 15)
        self.assertEqual(report["ui_pending_replacements"], 40)
        self.assertEqual(report["ui_cache_sync_errors"], 0)
        self.assertEqual(report["ui_stride_bytes"], 512)
        self.assertIsNone(common.flatten_status({})["camera_ui_source_updates"])

    def test_preview_transient_evidence_survives_polling_and_counter_reset(self):
        samples = []
        for elapsed, checks, mismatches, jumps in ((0, 100, 0, 0), (10, 200, 1, 2)):
            status = common.flatten_status({"camera": {"frames": checks, "ui_preview": {
                "integrity_checks": checks, "integrity_mismatches": mismatches,
                "pink_jump_candidates": jumps, "last_pink_frame": 150,
                "last_pink_before_rgb": 0x808080, "last_pink_after_rgb": 0xff40ff}}})
            samples.append({"elapsed": elapsed, "status": status})
        report = modes.camera_processing_report(samples)
        self.assertEqual(report["ui_integrity_checks_delta"], 100)
        self.assertEqual(report["ui_integrity_mismatches_delta"], 1)
        self.assertEqual(report["ui_pink_jump_candidates_delta"], 2)
        self.assertEqual(report["ui_last_pink_after_rgb"], 0xff40ff)
        samples[-1]["status"]["camera_ui_integrity_checks"] = 1
        self.assertIsNone(modes.camera_processing_report(samples)["ui_integrity_checks_delta"])

    def test_old_firmware_has_no_preview_integrity_evidence(self):
        samples = [{"elapsed": t, "status": common.flatten_status({"camera": {"frames": t + 1}})}
                   for t in (0, 10)]
        report = modes.camera_processing_report(samples)
        self.assertIsNone(report["ui_integrity_mismatches"])
        self.assertIsNone(report["ui_integrity_checks_delta"])
        self.assertIsNone(report["ui_pink_jump_candidates_delta"])

    def test_dns_category_is_counted_beyond_error_example_limit(self):
        rows = [{"ok": False, "http_status": 0, "error": "connection reset",
                 "error_category": "connection_reset", "elapsed_ms": 10} for _ in range(5)]
        rows.append({"ok": False, "http_status": 0, "error": "getaddrinfo failed",
                     "error_category": "dns_or_mdns", "elapsed_ms": 5000})
        metrics = modes.summarize_requests(rows, {}, 120, 2)
        self.assertEqual(metrics["dns_errors"], 1)
        self.assertEqual(metrics["transport_errors"], 5)
        self.assertEqual(evaluate(metrics)["observation_issue"], "dns_or_mdns")

    def test_recovered_dns_retries_do_not_hide_pressure_measurement_interference(self):
        rows = [{"ok": True, "http_status": 200, "elapsed_ms": 10, "timing": {"dns_lookup_failures": 1}} for _ in range(10)]
        result = evaluate(modes.summarize_requests(rows, {}, 5, 2))
        self.assertEqual(result["verdict"], "inconclusive")
        self.assertEqual(result["observation_issue"], "dns_or_mdns")

    def test_dns_uncertainty_keeps_real_device_failure_evidence(self):
        rows = [{"ok": False, "http_status": 0, "error": "getaddrinfo failed", "elapsed_ms": 5000},
                {"ok": False, "http_status": 429, "error": "HTTP 429: ESP_ERR_TIMEOUT", "elapsed_ms": 10}]
        result = evaluate(modes.summarize_requests(rows, {}, 1, 2))
        self.assertEqual(result["verdict"], "limit")
        self.assertEqual(result["limit_kind"], "firmware_queue_rejection")
        self.assertIn("dns_or_mdns_errors", result["reasons"])

    def test_peer_closure_under_delivered_load_is_connection_pressure(self):
        self.assertEqual(common.classify_http_error("[WinError 10054] 远程主机强迫关闭了一个现有的连接。"), "connection_reset")
        self.assertEqual(modes.pressure_error_category({"error_category": "other", "error": "[WinError 10054] 远程主机强迫关闭了一个现有的连接。"}), "connection_reset")
        rows = [{"ok": True, "http_status": 200, "elapsed_ms": 10} for _ in range(90)]
        rows += [{"ok": False, "http_status": 0, "error_category": "other",
                  "error": "Remote end closed connection without response", "elapsed_ms": 10} for _ in range(10)]
        metrics = modes.summarize_requests(rows, {}, 10, 10)
        result = evaluate(metrics, delivery_loss=True, recovered=True)
        self.assertEqual(metrics["peer_closed_errors"], 10)
        self.assertEqual(result["verdict"], "limit")
        self.assertEqual(result["limit_kind"], "http_connection_pressure")
        self.assertNotIn("recovery_failed", result["reasons"])
        metrics["dns_errors"] = 1
        self.assertEqual(evaluate(metrics)["verdict"], "inconclusive")

    def test_event_history_fill_requires_clean_execution_and_preserved_internal_heap(self):
        before = {"capacity": 1024, "published": 231}
        after = {"capacity": 1024, "published": 1892}
        _, drops = modes.heap_recovered({"internal_free": 175811, "internal_largest": 126976,
                                        "psram_free": 11912884, "psram_largest": 11796480},
                                       {"internal_free": 175811, "internal_largest": 126976,
                                        "psram_free": 10763172, "psram_largest": 10223616}, 32768, .1)
        metrics = {**metric(), "commands_accepted": 233}
        self.assertTrue(modes.event_history_warmup_candidate(before, after, drops, metrics, True, True))
        self.assertFalse(modes.event_history_warmup_candidate(before, after, drops, metrics, True, False))
        self.assertFalse(modes.event_history_warmup_candidate({**before, "published": 1200}, after, drops, metrics, True, True))
        self.assertFalse(modes.event_history_warmup_candidate(before, after, drops, {**metrics, "error_rate": .01}, True, True))
        drops["internal_free"]["drop"] = 40000
        self.assertFalse(modes.event_history_warmup_candidate(before, after, drops, metrics, True, True))

    def test_status_error_during_load_does_not_fail_successful_recovery(self):
        board = Board()
        with patch.object(common, "HttpClient", board.client_class()), contextlib.redirect_stdout(io.StringIO()):
            session = modes.Session(args_for(scratch(), ["--mode", "stress"]), common)
            try:
                session.tick(force=True)
                original_tick = session.tick
                injected = []
                def tick(*args, **kwargs):
                    original_tick(*args, **kwargs)
                    if session.phase.endswith("_probe") and not injected:
                        injected.append(True)
                        session.health_failures += 1
                with patch.object(session, "tick", side_effect=tick):
                    row = session.load_stage({"axis": "http", "level": 1, "clients": 1, "value": 1, "per_client_rps": 30})
                self.assertTrue(row["recovered"])
                self.assertTrue(row["recovery"]["monitoring_restored"])
                self.assertEqual(row["recovered"], row["recovery"]["recovered"])
                self.assertNotIn("recovery_failed", row["reasons"])
                self.assertEqual(row["verdict"], "inconclusive")
            finally:
                for log in session.logs.values():
                    log.close()

    def test_dns_only_does_not_block_independent_warmup_repeat(self):
        before = {"capacity": 1024, "published": 250}
        after = {"capacity": 1024, "published": 1727}
        _, drops = modes.heap_recovered(
            {"internal_free": 169311, "internal_largest": 126976, "psram_free": 11859476, "psram_largest": 11796480},
            {"internal_free": 169311, "internal_largest": 126976, "psram_free": 10727208, "psram_largest": 10485760}, 32768, .1)
        metrics = {**metric(), "commands_accepted": 205, "errors": 2, "dns_errors": 2,
                   "error_rate": 2 / 207, "response_error_rate": 0}
        self.assertTrue(modes.event_history_warmup_candidate(before, after, drops, metrics, True, True))
        self.assertFalse(modes.event_history_warmup_candidate(before, after, drops, metrics, True, False))
        metrics["transport_errors"] = 1
        self.assertFalse(modes.event_history_warmup_candidate(before, after, drops, metrics, True, True))

    def test_heap_failure_is_not_reported_as_missing_network_observation(self):
        row = {"recovery": {"idle": True, "monitoring_restored": True, "heap_within_threshold": False}}
        self.assertEqual(modes.recovery_failure_reason(row, "recovery_unobserved"), "heap_recovery_failed")
        row["recovery"]["monitoring_restored"] = False
        self.assertEqual(modes.recovery_failure_reason(row, "recovery_unobserved"), "recovery_unobserved")

    def test_warmup_dns_evidence_survives_a_successful_repeat(self):
        session = modes.Session(args_for(scratch(), ["--mode", "stress", "--stress-axes", "commands", "--stress-clients", "1"]), common)
        def stage(load, confirmation=False, warmup_repeat=False):
            if confirmation or warmup_repeat:
                return {"load": load, "verdict": "pass", "recovered": True}
            return {"load": load, "verdict": "inconclusive", "recovered": False,
                    "warmup_candidate": True, "observation_issue": "dns_or_mdns"}
        try:
            self.assertEqual(session.http.resolver.ttl, float("inf"))
            with patch.object(session, "load_stage", side_effect=stage), patch.object(session, "save"):
                session.run_stress()
            self.assertTrue(session.boundaries["commands"]["confirmed"])
            self.assertEqual(session.summary.stop_reason, "boundary_search_inconclusive")
            self.assertEqual(session.boundaries["commands"]["warmup_observation_issue"], "dns_or_mdns")
        finally:
            for log in session.logs.values():
                log.close()

    def test_warmup_gets_one_repeat_and_persistent_heap_drop_still_stops(self):
        for repeat_passes in (True, False):
            session = modes.Session(args_for(scratch(), ["--mode", "stress", "--stress-axes", "commands", "--stress-clients", "1"]), common)
            calls = []
            def stage(load, confirmation=False, warmup_repeat=False):
                calls.append((confirmation, warmup_repeat))
                if confirmation:
                    return {"load": load, "verdict": "pass", "recovered": True}
                if warmup_repeat:
                    return {"load": load, "verdict": "pass" if repeat_passes else "limit", "recovered": repeat_passes}
                return {"load": load, "verdict": "inconclusive", "recovered": False, "warmup_candidate": True}
            try:
                with patch.object(session, "load_stage", side_effect=stage), patch.object(session, "save"):
                    if repeat_passes:
                        session.run_stress()
                    else:
                        with self.assertRaises(modes.StopRun):
                            session.run_stress()
                boundary = session.boundaries["commands"]
                self.assertIn("warmup_stage", boundary)
                self.assertEqual(sum(warm for _, warm in calls), 1)
                if repeat_passes:
                    self.assertTrue(boundary["confirmed"])
                    self.assertIsNone(boundary["first_limit"])
                else:
                    self.assertEqual(session.summary.stop_reason, "recovery_failed")
                    self.assertEqual(boundary["interpretation"], "recovery_failed")
            finally:
                for log in session.logs.values():
                    log.close()

    def test_last_passing_level_is_confirmed_after_recovered_observation_failure(self):
        session = modes.Session(args_for(scratch(), ["--mode", "stress", "--stress-axes", "http", "--stress-clients", "1,2"]), common)
        calls = []
        def stage(load, confirmation=False):
            calls.append((load["value"], confirmation))
            return {"load": load, "verdict": "inconclusive" if load["value"] == 2 else "pass", "recovered": True}
        try:
            with patch.object(session, "load_stage", side_effect=stage), patch.object(session, "save"):
                session.run_stress()
            self.assertEqual(calls[-1], (1, True))
            self.assertTrue(session.boundaries["http"]["confirmed"])
            self.assertEqual(session.boundaries["http"]["interpretation"], "load_or_observation_inconclusive")
        finally:
            for log in session.logs.values():
                log.close()

    def test_inconclusive_stage_is_not_saved_as_first_limit(self):
        session = modes.Session(args_for(scratch(), ["--mode", "stress", "--stress-axes", "http"]), common)
        try:
            with patch.object(session, "load_stage", side_effect=lambda load, confirmation=False: {
                    "load": load, "verdict": "inconclusive", "recovered": True, "observation_issue": "dns_or_mdns"}), patch.object(session, "save"):
                session.run_stress()
            boundary = session.boundaries["http"]
            self.assertIsNone(boundary["first_limit"])
            self.assertFalse(boundary["boundary_found"])
            self.assertFalse(boundary["confirmed"])
            self.assertEqual(boundary["inconclusive_stage"]["observation_issue"], "dns_or_mdns")
            self.assertEqual(boundary["interpretation"], "load_or_observation_inconclusive")
            self.assertEqual(session.summary.stop_reason, "boundary_search_inconclusive")
            with patch.object(session, "save"), contextlib.redirect_stdout(io.StringIO()):
                session.finish()
            self.assertEqual(session.result, "inconclusive")
        finally:
            for log in session.logs.values():
                log.close()

    def test_later_success_does_not_erase_an_unresolved_functional_case(self):
        session = modes.Session(args_for(scratch(), ["--functional-cases", "status,camera"]), common)
        try:
            session.cases = [{"cycle": 1, "case": "status", "verdict": "pass"},
                             {"cycle": 1, "case": "camera", "verdict": "pass"},
                             {"cycle": 2, "case": "camera", "verdict": "inconclusive", "failure_category": "artifact_transport"},
                             {"cycle": 3, "case": "camera", "verdict": "pass"}]
            session.summary.stop_reason = "duration_complete"
            with patch.object(session, "resource_report", return_value={"stability": "within_threshold"}), patch.object(session, "save"), contextlib.redirect_stdout(io.StringIO()):
                session.finish()
                self.assertEqual(session.result, "inconclusive")
                session.cases.pop(2)
                session.cases.append({"cycle": 4, "case": "camera", "verdict": "inconclusive",
                                      "failure_category": "test_interrupted", "stop_reason": "duration_complete"})
                session.finish()
                self.assertEqual(session.result, "pass")  # Only the planned deadline tail is exempt.
        finally:
            for log in session.logs.values():
                log.close()

    def test_heap_recovery_and_secret_redaction(self):
        ok, drops = modes.heap_recovered({"internal_free": 200000}, {"internal_free": 100000}, 32768, .1)
        self.assertFalse(ok)
        self.assertEqual(drops["internal_free"]["drop"], 100000)
        secret = {"model": {"api_key": "private", "max_tokens": 512}, "network": {"sta": {"password": "password"}}}
        result = modes.redact(secret)
        self.assertNotIn("private", json.dumps(result))
        self.assertEqual(result["model"]["max_tokens"], 512)

    def test_uptime_is_carried_forward_for_reboot_detection(self):
        summary = common.Summary()
        previous = summary.observe_status({"uptime_ms": 30000}, None)
        summary.observe_status({"uptime_ms": 1000}, previous)
        self.assertEqual(summary.reboots_observed, 1)

    def test_two_clients_execute_in_parallel_with_telemetry_and_completion(self):
        board = Board()
        board.request_barrier = threading.Barrier(2, action=lambda: setattr(board, "request_barrier", None))
        args = args_for(scratch(), ["--mode", "stress"])
        with patch.object(common, "HttpClient", board.client_class()), contextlib.redirect_stdout(io.StringIO()):
            session = modes.Session(args, common)
            try:
                session.tick(force=True)
                row = session.load_stage({"axis": "commands", "level": 1, "clients": 2, "value": 2, "per_client_rps": 30})
                self.assertEqual(row["verdict"], "pass", row)
                self.assertGreaterEqual(board.max_active, 2)
                self.assertGreater(board.status_reads, 2)
                self.assertGreater(row["metrics"]["commands_completed"], 5)
                self.assertEqual(row["workers_still_running"], 0)
            finally:
                for log in session.logs.values():
                    log.close()

    def test_functional_storage_and_script_execution_have_device_evidence(self):
        board = Board()
        with patch.object(common, "HttpClient", board.client_class()):
            session = modes.Session(args_for(scratch()), common)
            try:
                for name in ("lua", "file_io", "lua_file"):
                    result = session.functional_case(name, 1)
                    self.assertIn("evidence", result)
                self.assertEqual(len(board.files), 2)
                self.assertTrue(all("CCT_" in key for key in board.files))
            finally:
                for log in session.logs.values():
                    log.close()

    def test_boundary_search_confirms_last_pass_and_does_not_invent_maximum(self):
        session = modes.Session(args_for(scratch(), ["--mode", "stress", "--stress-axes", "http", "--stress-clients", "1,2,4"]), common)
        calls = []
        def fake_stage(stage, confirmation=False):
            calls.append((stage["value"], confirmation))
            return {"load": stage, "verdict": "limit" if stage["value"] == 4 else "pass", "recovered": True}
        try:
            with patch.object(session, "load_stage", side_effect=fake_stage):
                session.run_stress()
            b = session.boundaries["http"]
            self.assertEqual(b["last_passing"]["load"]["value"], 2)
            self.assertEqual(b["first_limit"]["load"]["value"], 4)
            self.assertTrue(b["confirmed"])
            self.assertEqual(calls[-1], (2, True))
            with patch.object(session, "load_stage", side_effect=lambda stage, confirmation=False: {"load": stage, "verdict": "pass", "recovered": True}):
                session.run_stress()
            self.assertFalse(session.boundaries["http"]["boundary_found"])
            self.assertEqual(session.boundaries["http"]["interpretation"], "lower_bound_only_limit_not_reached")
        finally:
            for log in session.logs.values():
                log.close()

    def test_full_functional_mode_deadline_saves_inconclusive_instead_of_pass(self):
        board = Board()
        args = args_for(scratch(), ["--duration", ".2s"])
        with patch.object(common, "HttpClient", board.client_class()), contextlib.redirect_stdout(io.StringIO()):
            session = modes.Session(args, common)
            code = session.run()
        result = json.loads((session.output / "summary.json").read_text(encoding="utf-8"))
        self.assertEqual(code, 1)
        self.assertEqual(result["stop_reason"], "duration_complete")
        self.assertEqual(result["verdict"], "inconclusive")
        self.assertTrue((session.output / "summary_report.txt").exists())

    def test_report_lock_is_retried_and_does_not_abort_observation(self):
        session = modes.Session(args_for(scratch()), common)
        try:
            session.save(final=True)
            original = (session.output / "summary.json").read_bytes()
            with patch.object(Path, "replace", side_effect=PermissionError(5, "locked")) as replace, patch.object(modes.time, "sleep"):
                session.save(final=True)
            self.assertEqual(replace.call_count, 4)
            self.assertEqual(session.report_write_errors, 1)
            self.assertEqual((session.output / "summary.json").read_bytes(), original)
            primary = session.output / "summary.json"
            fallback = session.output / "summary.final.json"
            same_time = primary.stat().st_mtime_ns
            os.utime(fallback, ns=(same_time, same_time))
            self.assertEqual(dashboard.read_summary(session.output)["report_write_errors"], 1)
            session.save(final=True)
            self.assertEqual(json.loads((session.output / "summary.json").read_text(encoding="utf-8"))["report_write_errors"], 1)
            os.utime(primary, ns=(same_time, same_time))
            self.assertEqual(dashboard.read_summary(session.output)["report_sequence"], session.report_sequence)
        finally:
            for log in session.logs.values():
                log.close()

    def test_startup_dns_retry_is_bounded_and_read_only(self):
        session = modes.Session(args_for(scratch()), common)
        try:
            with patch.object(session, "json_request", side_effect=[OSError("getaddrinfo failed"), '{"ok":true}']) as request, patch.object(session.http, "close"), patch.object(modes.time, "sleep"):
                self.assertEqual(session.startup_read("/api/config"), {"ok": True})
                self.assertTrue(all(c.args[0] == "GET" for c in request.call_args_list))
            with patch.object(session, "json_request", side_effect=OSError("getaddrinfo failed")) as request, patch.object(session.http, "close"), patch.object(modes.time, "sleep"):
                with self.assertRaises(modes.StopRun):
                    session.startup_read("/api/config")
                self.assertEqual(request.call_count, 3)
                self.assertEqual(session.summary.stop_reason, "startup_connection_failed")
        finally:
            for log in session.logs.values():
                log.close()

    def test_transport_failure_drains_run_and_preserves_camera_success(self):
        session = modes.Session(args_for(scratch()), common)
        try:
            session.status = {"interaction": {"busy": False, "pending": 0}}
            def advance(_):
                if not session.runtime_errors:
                    session.runtime_errors.append("HTTP_CLIENT: Incomplete chunked data")
                    session.status["interaction"]["busy"] = True
                    for name, status in [("tool.start", "started"), ("tool.finish", "ok")]:
                        session.event_tail.append({"seq": len(session.event_tail) + 1, "kind": "profile", "text": json.dumps({
                            "sdk_event": name, "status": status, "attributes": {"tool": "camera_capture", "tool_call_id": "camera-1", "ok": True}})})
                else:
                    session.status["interaction"]["busy"] = False
                    session.event_tail.append({"seq": 3, "kind": "run_state", "text": "idle"})
            with patch.object(session, "tick"), patch.object(session, "json_request"), patch.object(session, "wait", side_effect=advance) as wait:
                with self.assertRaises(modes.CaseFailure) as failure:
                    session.model_case("camera", required_tool="camera_capture")
            self.assertEqual(wait.call_count, 2)
            self.assertEqual(failure.exception.category, "transport")
            self.assertTrue(failure.exception.evidence["tool_execution"]["succeeded"])
        finally:
            for log in session.logs.values():
                log.close()

    def test_tool_error_status_survives_missing_message(self):
        session = modes.Session(args_for(scratch()), common)
        try:
            event = {"kind": "profile", "text": json.dumps({"sdk_event": "tool.error", "status": "limit_exceeded",
                "attributes": {"tool": "memory", "tool_call_id": "m1", "ok": False}})}
            self.assertEqual(session._tool_execution_evidence([event], "memory")["errors"], ["limit_exceeded"])
        finally:
            for log in session.logs.values():
                log.close()

    def test_camera_requires_own_file_and_memory_reports_tool_execution(self):
        session = modes.Session(args_for(scratch()), common)
        try:
            with patch.object(session, "model_case", return_value={}) as model, patch.object(session.http, "request_bytes", return_value=(200, b"\xff\xd8jpeg\xff\xd9", .01)) as download:
                result = session.functional_case("camera", 1)
            self.assertIn("CCT_simulated_1_camera.jpg", download.call_args.kwargs["query"]["path"])
            self.assertEqual(download.call_args.args, ("GET", "/api/file"))
            self.assertEqual(download.call_args.kwargs["max_bytes"], 1024 * 1024)
            self.assertIn("save_path", model.call_args.args[0])
            self.assertEqual(result["jpeg_bytes"], 8)
            with patch.object(session, "model_case", return_value={}):
                result = session.functional_case("memory", 1)
            self.assertEqual(result["verdict"], "pass")
        finally:
            for log in session.logs.values():
                log.close()

    def test_camera_download_retries_only_get_and_preserves_execution_evidence(self):
        session = modes.Session(args_for(scratch()), common)
        try:
            evidence = {"tool_execution": {"succeeded": True}}
            with patch.object(session, "model_case", return_value=evidence) as model, patch.object(session.http, "request_bytes", side_effect=[
                    OSError("getaddrinfo failed"), (200, b"\xff\xd8jpeg\xff\xd9", .01)]) as download:
                result = session.functional_case("camera", 1)
                self.assertEqual(model.call_count, 1)
                self.assertEqual(download.call_count, 2)
                self.assertEqual(result["download_errors"], ["getaddrinfo failed"])
            with patch.object(session, "model_case", return_value=evidence) as model, patch.object(session.http, "request_bytes", side_effect=OSError("getaddrinfo failed")) as download:
                with self.assertRaises(modes.CaseFailure) as failed:
                    session.functional_case("camera", 2)
                self.assertEqual(model.call_count, 1)
                self.assertEqual(download.call_count, 2)
                self.assertEqual(failed.exception.category, "artifact_transport")
                self.assertTrue(failed.exception.evidence["tool_execution"]["succeeded"])
        finally:
            for log in session.logs.values():
                log.close()

    def test_tool_evidence_requires_execution_finish(self):
        session = modes.Session(args_for(scratch()), common)
        try:
            events = [
                {"kind": "tool", "text": "tool start memory"},
                {"kind": "profile", "text": json.dumps({
                    "sdk_event": "tool.start",
                    "attributes": {"tool": "memory", "tool_call_id": "call-1"},
                })},
                {"kind": "profile", "text": json.dumps({
                    "sdk_event": "tool.finish", "status": "ok",
                    "attributes": {"tool": "memory", "tool_call_id": "call-1", "ok": True},
                })},
            ]
            evidence = session._tool_execution_evidence(events, "memory")
            self.assertTrue(evidence["started"])
            self.assertTrue(evidence["finished"])
            self.assertTrue(evidence["succeeded"])
            self.assertEqual(evidence["tool_call_ids"], ["call-1"])
        finally:
            for log in session.logs.values():
                log.close()

    def test_completed_reasoning_only_run_fails_without_waiting_for_timeout(self):
        session = modes.Session(args_for(scratch()), common)
        try:
            session.status = {"interaction": {"busy": False, "pending": 0}}
            session.event_tail.append({"seq": session.cursor + 1, "kind": "run_state", "text": "idle"})
            with patch.object(session, "tick"), patch.object(session, "json_request"), patch.object(session, "wait") as wait:
                with self.assertRaisesRegex(RuntimeError, "completed without answer text"):
                    session.model_case("test")
            self.assertEqual(wait.call_count, 1)
        finally:
            for log in session.logs.values():
                log.close()

    def test_truncated_profile_preserves_camera_execution_failure(self):
        session = modes.Session(args_for(scratch()), common)
        try:
            events = []
            for phase, status, ok in [("tool.start", "started", True), ("tool.finish", "error", False)]:
                prefix = json.dumps({"event": "sdk_observability", "sdk_event": phase,
                    "status": status, "attributes": {"tool": "camera_capture",
                    "tool_call_id": "camera-1", "args": '{"quality":50}', "ok": ok}})
                events.append({"kind": "profile", "text": prefix[:-1] + ', "heap":{"free":123'})
            evidence = session._tool_execution_evidence(events, "camera_capture")
            self.assertTrue(evidence["started"])
            self.assertTrue(evidence["finished"])
            self.assertFalse(evidence["succeeded"])
            self.assertEqual(evidence["error_ids"], ["camera-1"])
            self.assertIn("tool returned failure", evidence["errors"])
        finally:
            for log in session.logs.values():
                log.close()

    def test_truncated_attributes_do_not_invent_tool_success(self):
        session = modes.Session(args_for(scratch()), common)
        try:
            evidence = session._tool_execution_evidence([{"kind": "profile", "text":
                '{"sdk_event":"tool.finish","status":"ok","attributes":{"tool":"camera_capture",'}], "camera_capture")
            self.assertFalse(evidence["succeeded"])
        finally:
            for log in session.logs.values():
                log.close()

    def test_tool_evidence_rejects_unrelated_finish_id(self):
        session = modes.Session(args_for(scratch()), common)
        try:
            events = [
                {"kind": "profile", "text": json.dumps({
                    "sdk_event": "tool.start",
                    "attributes": {"tool": "memory", "tool_call_id": "call-1"},
                })},
                {"kind": "profile", "text": json.dumps({
                    "sdk_event": "tool.finish", "status": "ok",
                    "attributes": {"tool": "memory", "tool_call_id": "call-2", "ok": True},
                })},
            ]
            evidence = session._tool_execution_evidence(events, "memory")
            self.assertFalse(evidence["succeeded"])
            self.assertIn("tool execution result id did not match tool start", evidence["errors"])
        finally:
            for log in session.logs.values():
                log.close()


if __name__ == "__main__":
    unittest.main()
