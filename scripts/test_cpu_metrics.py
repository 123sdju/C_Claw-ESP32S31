"""Host-only regression checks for CPU profile normalization and schema handling."""

import unittest
import json
from unittest.mock import patch

import long_run_dashboard as dashboard
import long_run_test as runner
import resource_budget_report as budget_report


def current_cpu_payload():
    return {
        "available": True,
        "runtime_stats": True,
        "valid": True,
        "window_ms": 5000,
        "cores": 2,
        "total_pct": 26.5,
        "core_pct": [3.5, 49.5],
        "tasks": [
            {"id": 1, "n": "IDLE0", "c": 0, "p": 25.0, "core_p": 50.0},
            {"id": 2, "n": "IDLE1", "c": 1, "p": 20.0, "core_p": 40.0},
        ],
    }


class CpuMetricTests(unittest.TestCase):
    def test_legacy_cpu_frame_is_not_accepted_as_valid(self):
        legacy = {
            "available": True,
            "runtime_stats": True,
            "td": 5000000,
            "core_pct": [0, 0],
            "tasks": [{"n": "IDLE0", "p": 96.0, "cp": 100.0}],
        }
        self.assertFalse(runner.cpu_frame_is_valid(legacy))
        self.assertFalse(dashboard.cpu_frame_is_valid(legacy))
        self.assertFalse(budget_report.cpu_frame_is_valid(legacy))

    def test_current_cpu_frame_uses_explicit_window(self):
        cpu = current_cpu_payload()
        self.assertTrue(runner.cpu_frame_is_valid(cpu))
        self.assertTrue(dashboard.cpu_frame_is_valid(cpu))

    def test_summary_keeps_per_core_and_global_cpu_scopes_separate(self):
        summary = runner.Summary()
        summary.observe_profile_payload({"phase": "interval", "cpu": current_cpu_payload()})
        result = summary.as_dict()

        self.assertEqual(result["profile_cpu_quality"]["valid_samples"], 1)
        self.assertEqual(result["profile_cpu_quality"]["legacy_samples"], 0)
        self.assertEqual(result["profile_cpu_total"]["avg_pct"], 26.5)
        self.assertEqual(result["profile_cpu_cores"]["0"]["avg_pct"], 3.5)
        self.assertEqual(result["profile_cpu_cores"]["1"]["avg_pct"], 49.5)
        self.assertEqual(result["profile_cpu_tasks"]["id:1:IDLE0"]["avg_pct"], 25.0)

    def test_dashboard_does_not_plot_legacy_cp_as_per_core_usage(self):
        self.assertIsNone(
            dashboard.cpu_task_value({"n": "worker", "c": 0, "p": 30.0, "cp": 100.0})
        )
        self.assertEqual(
            dashboard.cpu_task_value({"n": "worker", "c": 0, "p": 30.0, "core_p": 30.0}),
            ("worker", 30.0, 0),
        )

    def test_summary_deduplicates_cached_cpu_window(self):
        summary = runner.Summary()
        payload = {"phase": "periodic", "timestamp_ms": 5000,
                   "cpu": {**current_cpu_payload(), "sample_seq": 7}}
        summary.observe_profile_payload(payload)
        # The cached CPU window is embedded in later profile events whose
        # outer timestamp changes. sample_seq must still deduplicate it.
        summary.observe_profile_payload({**payload, "timestamp_ms": 6000})
        result = summary.as_dict()
        self.assertEqual(result["profile_cpu_quality"]["valid_samples"], 1)
        self.assertEqual(result["profile_cpu_quality"]["duplicate_samples"], 1)
        self.assertEqual(result["profile_cpu_total"]["samples"], 1)

    def test_resource_report_deduplicates_cached_cpu_window(self):
        budget = budget_report.Budget()
        payload = {"timestamp_ms": 5000,
                   "cpu": {**current_cpu_payload(), "sample_seq": 7}}
        budget_report.observe_profile_payload(payload, budget)
        budget_report.observe_profile_payload({**payload, "timestamp_ms": 6000}, budget)
        self.assertEqual(budget.cpu_valid_samples, 1)
        self.assertEqual(budget.cpu_duplicate_samples, 1)

    def test_idle_starvation_is_reported_as_a_streak(self):
        summary = runner.Summary()
        for sequence in (1, 2, 3):
            summary.observe_profile_payload({
                "timestamp_ms": sequence * 5000,
                "cpu": {**current_cpu_payload(), "sample_seq": sequence,
                        "core_idle_pct": [0.5, 50.0]},
            })
        quality = summary.as_dict()["profile_cpu_quality"]
        self.assertEqual(quality["idle_low_samples"]["0"], 3)
        self.assertEqual(quality["idle_low_max_streak"]["0"], 3)
        self.assertEqual(quality["idle_low_streak"]["0"], 3)

    def test_idle_missing_is_separate_from_measured_zero_idle(self):
        summary = runner.Summary()
        cpu = {**current_cpu_payload(), "sample_seq": 1,
               "core_idle_pct": [None, 50.0], "core_pct": [None, 50.0]}
        summary.observe_profile_payload({"timestamp_ms": 5000, "cpu": cpu})
        summary.observe_profile_payload({
            "timestamp_ms": 6000,
            "cpu": {**current_cpu_payload(), "sample_seq": 2,
                    "valid": False, "reason": "core_idle_missing",
                    "core_idle_pct": [None, 50.0]},
        })
        quality = summary.as_dict()["profile_cpu_quality"]
        self.assertEqual(quality["idle_missing_samples"]["0"], 1)
        self.assertEqual(quality["idle_unavailable_samples"], 1)

    def test_dashboard_idle_quality_uses_explicit_idle_field(self):
        quality = {
            "invalid_value_count": 0,
            "idle_missing_samples": {},
            "idle_low_samples": {},
            "idle_low_max_streak": {},
        }
        streak = {}
        dashboard.update_cpu_idle_quality(
            {"cores": 2, "core_idle_pct": [0.25, None], "core_pct": [99.75, None]},
            quality,
            streak,
        )
        self.assertEqual(quality["idle_low_samples"]["0"], 1)
        self.assertEqual(quality["idle_missing_samples"]["1"], 1)

    def test_resource_report_keeps_idle_starvation_evidence(self):
        budget = budget_report.Budget()
        cpu = {**current_cpu_payload(), "sample_seq": 1,
               "core_idle_pct": [0.0, 50.0]}
        budget_report.observe_profile_payload({"timestamp_ms": 5000, "cpu": cpu}, budget)
        data = budget_report.as_dict(budget)["runtime"]
        self.assertEqual(data["cpu_idle_low_samples"]["0"], 1)
        self.assertEqual(data["cpu_idle_low_max_streak"]["0"], 1)

    def test_dashboard_deduplicates_cached_window_with_new_outer_timestamp(self):
        cpu = {**current_cpu_payload(), "sample_seq": 7}
        lines = [
            {"event": {"kind": "profile", "text": json.dumps({"timestamp_ms": ts, "cpu": cpu})}}
            for ts in (5000, 6000)
        ]
        with patch.object(dashboard, "read_tail", return_value=[json.dumps(line) for line in lines]):
            result = dashboard.read_cpu_series(object())
        self.assertEqual(result["quality"]["valid_samples"], 1)
        self.assertEqual(result["quality"]["duplicate_samples"], 1)


if __name__ == "__main__":
    unittest.main()
