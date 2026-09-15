"""长时测试器的拓扑判定, 输出边界与有界采样, 不需要原生服务或下载工具."""

from copy import deepcopy
import json
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from testkit.support import ROOT, temporary_directory

sys.path.insert(0, str(ROOT / "astra"))
from test_endurance import Endurance, Journal, RequestedStop, parse_options, topology_ready


class EnduranceTests(unittest.TestCase):
    def topology(self):
        return {
            "star-0": {"initialized": True, "id": "a", "members": 2, "inbound": 1, "outbound": 1, "planet_inbound": 1},
            "star-1": {"initialized": True, "id": "b", "members": 2, "inbound": 1, "outbound": 1, "planet_inbound": 1},
            "planet-0": {"initialized": True, "id": "p", "upstream": {"id": "a"}},
            "planet-1": {"initialized": True, "id": "q", "upstream": {"id": "b"}},
        }

    def test_balanced_topology_does_not_accept_wrong_owner_or_duplicate_identity(self):
        original = self.topology()
        self.assertTrue(topology_ready(original, 2, 2, 2))
        for node, key, value in (
            ("star-0", "id", "b"),
            ("star-0", "inbound", 0),
            ("star-0", "members", 3),
            ("planet-1", "upstream", {"id": "a"}),
            ("planet-1", "upstream", {"id": "retired"}),
        ):
            states = deepcopy(original)
            states[node][key] = value
            with self.subTest(node=node, key=key, value=value):
                self.assertFalse(topology_ready(states, 2, 2, 2))

    def test_rotating_journal_keeps_bounded_valid_recent_records(self):
        with temporary_directory("endurance-") as directory:
            journal = Journal(Path(directory), limit=1, slots=3)
            for number in range(10):
                journal.write({"number": number})
            files = list(Path(directory).glob("samples-*.jsonl"))
            self.assertEqual(len(files), 3)
            self.assertEqual(sorted(json.loads(path.read_text())["number"] for path in files), [7, 8, 9])

    def test_disconnected_planet_remains_pending_until_upstream_returns(self):
        # JSON null 是已准入 Planet 正在换绑的正常状态, 必须等待而不是抛异常或判定为已恢复.
        for upstream in (None, {}, {"id": "retired"}):
            with self.subTest(upstream=upstream):
                states = self.topology()
                states["planet-1"]["upstream"] = upstream
                self.assertFalse(topology_ready(states, 2, 2, 2))
                states["planet-1"]["upstream"] = {"id": "b"}
                self.assertTrue(topology_ready(states, 2, 2, 2))
        states = self.topology()
        del states["planet-1"]["upstream"]
        self.assertFalse(topology_ready(states, 2, 2, 2))

    def test_stop_request_does_not_change_processes_directly(self):
        run = Endurance.__new__(Endurance)
        with temporary_directory("endurance-") as directory:
            run.directory, run.stopping = Path(directory), False
            run.check_stop()
            (run.directory / "STOP").touch()
            with self.assertRaises(RequestedStop):
                run.check_stop()
        run.stopping = True
        with self.assertRaises(RequestedStop):
            run.check_stop()

    def test_stale_status_cannot_pass_using_cached_healthy_topology(self):
        run = Endurance.__new__(Endurance)
        run.check_stop = lambda: None
        run.directory = ROOT
        run.report = {"minimum_available_mib": 1024}
        run.fresh = {"star-0": (3, 10)}
        run.next_sample = float("inf")
        process = SimpleNamespace(snapshot=lambda: {"alive": True, "sequence": 3, "status": self.topology()["star-0"]})
        run.host = SimpleNamespace(processes={"star-0": process})
        with (
            patch("test_endurance.available_memory", return_value=1024**3),
            patch("test_endurance.os.statvfs", create=True, return_value=SimpleNamespace(f_bavail=1024**3, f_frsize=1)),
            patch("test_endurance.time.monotonic", return_value=21),
        ):
            with self.assertRaisesRegex(RuntimeError, "No fresh status"):
                run.snapshot()

    def test_output_and_duration_boundaries(self):
        output = str(ROOT / "build/endurance-tests")
        self.assertEqual(parse_options(["--output", output]).steady_seconds, 0)
        with patch("sys.stderr"):
            for arguments in (["--output", str(ROOT)], ["--output", output, "--stars", "1"], ["--output", output, "--fault-seconds", "-1"]):
                with self.subTest(arguments=arguments), self.assertRaises(SystemExit):
                    parse_options(arguments)


if __name__ == "__main__":
    unittest.main()
