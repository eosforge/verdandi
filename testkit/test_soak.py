"""长测入口的预算、证据和失败判定, 不启动集群或下载依赖."""

import contextlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from testkit.soak import Recorder, parse


class Soak(unittest.TestCase):
    def setUp(self):
        root = Path(__file__).resolve().parents[1] / "build/tmp"
        root.mkdir(parents=True, exist_ok=True)
        self.directory = tempfile.TemporaryDirectory(prefix="soak-unit-", dir=root)
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)

    def test_duration_bounds(self):
        for flags in (
            ["--fault-seconds", "-1"],
            ["--steady-seconds", "604801"],
            ["--fault-seconds", "604800", "--steady-seconds", "1"],
            ["--interval", "3601"],
            ["--records", "0"],
            ["--records", "17"],
        ):
            with self.subTest(flags=flags), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                parse(["--binaries", "build", "--output", "out", *flags])
        actual = parse(
            [
                "--binaries",
                "build",
                "--output",
                "out",
                "--fault-seconds",
                "0",
                "--steady-seconds",
                "0",
                "--interval",
                "0",
            ]
        )
        self.assertEqual((actual.fault_seconds, actual.steady_seconds, actual.interval), (0, 0, 0))

    def test_explicit_unbounded_steady(self):
        actual = parse(["--binaries", "build", "--output", "out", "--fault-seconds", "0", "--until-stopped"])
        self.assertIsNone(actual.steady_seconds)
        self.assertEqual(actual.fault_seconds, 0)
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            parse(["--binaries", "build", "--output", "out", "--until-stopped", "--steady-seconds", "10"])

    def test_progress_requires_new_confirmed_round(self):
        recorder = Recorder(self.root / "evidence", 0, None, 0)
        log = self.root / "steady.log"
        recorder.watch(log)
        start = recorder.progress[log]["updated"]
        recorder.observe(log, b'{"event":"mesh_progress","round":100}\n', start + 10)
        self.assertEqual(recorder.progress[log]["round"], 100)
        # 重复行、部分行和服务仍存活均不能掩盖业务停滞; 期限是单轮看门狗, 不是总时长.
        recorder.observe(log, b'{"event":"mesh_progress","round":100}\n{"event":', start + 100)
        self.assertEqual(recorder.progress[log]["updated"], start + 10)
        with self.assertRaisesRegex(RuntimeError, "no confirmed progress"):
            recorder.observe(log, b"", start + 191)
        with self.assertRaisesRegex(RuntimeError, "backwards"):
            recorder.observe(log, b'{"event":"mesh_progress","round":99}\n', start + 192)

    def test_existing_evidence_is_not_overwritten(self):
        with self.assertRaises(FileExistsError):
            Recorder(self.root, 0, 0, 0)

    def test_completion_and_interruption_are_distinct(self):
        recorder = Recorder(self.root / "evidence", 0, 0, 0)
        with contextlib.redirect_stdout(io.StringIO()):
            recorder.record("interrupted")
        values = [json.loads(line) for line in (recorder.output / "events.jsonl").read_text().splitlines()]
        self.assertEqual([item["event"] for item in values], ["interrupted"])

    def test_unexpected_even_clean_exit_fails(self):
        recorder = Recorder(self.root / "evidence", 0, 0, 0)
        process = Mock(pid=123)
        process.poll.return_value = 0
        recorder.expect(process, "star")
        with self.assertRaisesRegex(RuntimeError, "unexpectedly"):
            recorder.check([], force=True)
        recorder.retire(process)
        self.assertFalse(recorder.expected)

    @unittest.skipUnless(sys.platform == "linux", "Linux resource sampling")
    def test_incremental_sanitizer_marker_and_tail(self):
        recorder = Recorder(self.root / "evidence", 0, 0, 0)
        log = self.root / "service.log"
        log.write_bytes(b"ThreadSan")
        process = Mock(pid=os.getpid())
        process.poll.return_value = None
        recorder.check([(process, log)], force=True)
        self.assertEqual(recorder.offsets[log], 9)
        with log.open("ab") as stream:
            stream.write(b"itizer: data race\n")
        with self.assertRaisesRegex(RuntimeError, "Sanitizer"):
            recorder.check([(process, log)], force=True)
        self.assertIn(b"data race", (recorder.output / log.name).read_bytes())

    @unittest.skipUnless(sys.platform == "linux", "Linux resource sampling")
    def test_process_resources_and_sampling_throttle(self):
        recorder = Recorder(self.root / "evidence", 0, 0, 0)
        log = self.root / "service.log"
        log.write_bytes(b"ready\n")
        process = Mock(pid=os.getpid())
        process.poll.return_value = None
        recorder.check([(process, log)], force=True)
        recorder.check([(process, log)])
        records = [json.loads(line) for line in (recorder.output / "events.jsonl").read_text().splitlines()]
        self.assertEqual(len(records), 1)
        sample = records[0]["processes"][0]
        self.assertGreater(sample["rss"], 0)
        self.assertGreater(sample["threads"], 0)
        self.assertGreater(sample["fds"], 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
