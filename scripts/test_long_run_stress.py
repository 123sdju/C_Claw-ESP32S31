"""Host-only regression checks; never contacts a board or opens a serial port."""
import contextlib
import io
import json
import time
import unittest
from pathlib import Path
from unittest.mock import patch

import long_run_test as runner
import firmware_test_modes as modes


class Writer:
    def write(self, row):
        pass


class FakeHttp:
    instances = []

    def __init__(self, *args, **kwargs):
        self.base = "http://fake"
        self.last_timing = {}
        self.busy = False
        self.requests = []
        self.__class__.instances.append(self)

    def get_json(self, path, query=None):
        self.requests.append((path, query))
        if path == "/api/status":
            return {"uptime_ms": 10000, "interaction": {"busy": self.busy, "pending": 0}}, 0.001
        return {"latest_seq": 0, "events": []}, 0.001

    def post_message(self, text):
        if not text.startswith("/"):
            self.busy = True
        self.requests.append(("message", text))
        return 0.001

    def close(self):
        pass


class StressTests(unittest.TestCase):
    def test_preset_and_explicit_override(self):
        with patch.object(modes, "run_mode", return_value=0) as dry:
            runner.main(["--stress", "--dry-run", "--interval=3", "--no-serial"])
        args = dry.call_args.args[0]
        self.assertEqual(args.duration_seconds, 3600)
        self.assertEqual(args.interval, 3)
        self.assertEqual(args.conversation_interval, 2)
        self.assertNotIn("serial_commands", args.test_items_set)

    def test_reset_cursor_and_event_epoch(self):
        summary = runner.Summary()
        summary.observe_event_seq(1)
        payload = {"cursor_reset": True, "latest_seq": 2, "events": [{"seq": 1}, {"seq": 2}]}
        summary.observe_events_payload(payload)
        summary.observe_event_seq(1)
        self.assertEqual(runner.event_cursor(payload, 900), 2)
        self.assertEqual(summary.event_cursor_resets, 1)
        self.assertEqual(summary.event_duplicate_seq, 0)

    def test_truncated_answer_cannot_pass(self):
        http = FakeHttp()
        batch = [{"seq": 1, "kind": "user", "text": "test"},
                 {"seq": 2, "kind": "assistant", "text": "answer"},
                 {"seq": 3, "kind": "run_state", "text": "idle"}]
        http.get_json = lambda *args: ({"latest_seq": 3, "truncated": True, "omitted": 4, "events": batch}, 0.001)
        summary = runner.Summary()
        _, cursor, ok, timing = runner.wait_for_message_events(http, 0, 1, 0.01, Writer(), summary, 1000)
        self.assertFalse(ok)
        self.assertTrue(timing["log_incomplete"])
        self.assertEqual(cursor, 3)
        self.assertEqual(summary.event_omitted, 4)

    def test_reset_during_conversation_fails_promptly(self):
        http = FakeHttp()
        http.get_json = lambda *args: ({"latest_seq": 2, "cursor_reset": True, "events": []}, 0.001)
        _, cursor, ok, timing = runner.wait_for_message_events(http, 900, 30, 1, Writer(), runner.Summary(), 1000)
        self.assertFalse(ok)
        self.assertEqual(cursor, 0)
        self.assertEqual(timing["error_event_count"], 1)

    def test_duration_interrupts_stalled_model_and_writes_report(self):
        folder = Path(__file__).resolve().parents[1] / "tmp" / "long-run-regression" / str(time.time_ns())
        folder.mkdir(parents=True)
        with patch.object(modes, "run_mode", side_effect=lambda args, common: common.run(args)), patch.object(runner, "HttpClient", FakeHttp), patch.object(runner.signal, "signal"), contextlib.redirect_stdout(io.StringIO()):
            started = time.monotonic()
            code = runner.main(["--stress", "--no-serial", "--no-profile-guard", "--duration", "2.2s",
                                "--interval", "0.01", "--conversation-interval", "0.01", "--event-poll-interval", "0.01",
                                "--test-items", "status,events,network_commands,short_conversation", "--output-dir", str(folder)])
            elapsed = time.monotonic() - started
            summary = json.loads(next(Path(folder).glob("*/summary.json")).read_text())
            self.assertEqual(summary["stop_reason"], "duration_complete")
            self.assertGreaterEqual(summary["stress_status_samples"], 2)
            self.assertGreater(summary["stress_commands"], 0)
            self.assertLess(elapsed, 5)
            self.assertEqual(code, 1)  # No completed LLM round is not a successful stress run.
            self.assertTrue(next(Path(folder).glob("*/summary_report.txt")).exists())

    def test_stop_interrupts_both_waits(self):
        http = FakeHttp()
        def stop():
            raise runner.RunStopped()
        http.checkpoint = stop
        with self.assertRaises(runner.RunStopped):
            runner.wait_for_message_events(http, 0, 300, 2, Writer(), runner.Summary(), 1000)
        with self.assertRaises(runner.RunStopped):
            runner.wait_for_interaction_idle(http, 300, 2, runner.Summary())
        self.assertEqual(http.requests, [])

    def test_reboot_fails_exit_status(self):
        args = runner.build_parser().parse_args([])
        args.test_items_set = set()
        self.assertEqual(runner.run_exit_code(runner.Summary(reboots_observed=1), args), 1)


if __name__ == "__main__":
    unittest.main()
