"""Reject false recovery evidence and hidden diagnostics in the Pulsar process harness."""

from __future__ import annotations

import json
import io
from contextlib import redirect_stderr
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch

import test_pulsar as harness


class PulsarHarnessTests(unittest.TestCase):
    def setUp(self):
        root = harness.ROOT / "build/tmp"
        root.mkdir(parents=True, exist_ok=True)
        temporary = tempfile.TemporaryDirectory(prefix="pulsar-harness-", dir=root)
        self.addCleanup(temporary.cleanup)
        self.log = Path(temporary.name) / "service.log"

    def records(self, *samples):
        records = [{"event": "clock_status", "fields": fields} for fields in samples]
        self.log.write_text("".join(json.dumps(record) + "\n" for record in records), encoding="utf-8")

    def test_continuous_holdover_and_recovery(self):
        self.records(
            {"ready": False},
            {"ready": True, "nanoseconds": 100},
            {"ready": True, "nanoseconds": 100},
            {"ready": False, "nanoseconds": 110},
            {"ready": True, "nanoseconds": 120},
        )
        harness.verify_clock_log(self.log, require_holdover=True)

    def test_invalid_epoch_and_anchor_loss(self):
        for fields in (
            {"ready": True, "nanoseconds": 99},
            {"ready": False},
            {"ready": True, "nanoseconds": -1},
            {"ready": True, "nanoseconds": True},
        ):
            with self.subTest(fields=fields):
                self.records({"ready": True, "nanoseconds": 100}, fields)
                with self.assertRaises(RuntimeError):
                    harness.verify_clock_log(self.log)

    def test_missing_holdover_or_recovery_is_not_success(self):
        for samples in (
            ({"ready": False},),
            ({"ready": True, "nanoseconds": 100}, {"ready": True, "nanoseconds": 110}),
            ({"ready": True, "nanoseconds": 100}, {"ready": False, "nanoseconds": 110}),
        ):
            with self.subTest(samples=samples):
                self.records(*samples)
                with self.assertRaises(RuntimeError):
                    harness.verify_clock_log(self.log, require_holdover=True)

    def test_sanitizer_diagnostics_cannot_be_ignored_as_non_json(self):
        for marker in (
            "WARNING: ThreadSanitizer:",
            "ERROR: AddressSanitizer:",
            "ERROR: LeakSanitizer:",
            "runtime error:",
        ):
            with self.subTest(marker=marker):
                self.log.write_text(marker + " injected failure\n", encoding="utf-8")
                with self.assertRaisesRegex(RuntimeError, "sanitizer"):
                    harness.events(self.log)

    def test_non_json_and_incomplete_lines_do_not_create_events(self):
        self.records({"ready": True, "nanoseconds": 100})
        with self.log.open("a", encoding="utf-8") as output:
            output.write('ordinary diagnostic\n[1,2]\n{"event":')
        self.assertEqual(len(harness.events(self.log)), 1)

    def test_wait_does_not_reuse_an_earlier_ready_event(self):
        self.records({"ready": True, "nanoseconds": 100}, {"ready": False, "nanoseconds": 110})
        process = Mock()
        process.poll.return_value = None
        with patch.object(harness.time, "monotonic", side_effect=[0, 0, 1]), patch.object(harness.time, "sleep"):
            with self.assertRaises(TimeoutError):
                harness.wait(
                    process,
                    self.log,
                    lambda record: record["fields"]["ready"],
                    seconds=0.5,
                    overall_deadline=100,
                    after=1,
                )

    def test_wait_accepts_new_recovery_event(self):
        self.records({"ready": False, "nanoseconds": 100}, {"ready": True, "nanoseconds": 110})
        process = Mock()
        process.poll.return_value = None
        with patch.object(harness.time, "monotonic", side_effect=[0, 0]):
            value = harness.wait(
                process,
                self.log,
                lambda record: record["fields"]["ready"],
                seconds=0.5,
                overall_deadline=100,
                after=1,
            )
        self.assertEqual(value["fields"]["nanoseconds"], 110)

    def test_failure_keeps_original_error_and_bounded_log_tail(self):
        self.log.write_text("x" * 20000 + "\nuseful tail\n", encoding="utf-8")
        diagnostic = io.StringIO()
        with redirect_stderr(diagnostic):
            with self.assertRaisesRegex(ValueError, "original failure"):
                with harness.failure_logs(self.log.parent):
                    raise ValueError("original failure")
        self.assertIn("useful tail", diagnostic.getvalue())
        self.assertLess(len(diagnostic.getvalue()), 8300)


if __name__ == "__main__":
    unittest.main()
