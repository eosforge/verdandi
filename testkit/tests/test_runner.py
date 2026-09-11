"""Regression tests for ownership, cancellation, scope and bounded collectors."""

import contextlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import MagicMock, patch

from testkit import resources, support
from testkit.run import arguments, duration
from testkit.catalog.interop_test import start_peers
from testkit.sentinel.sentinel_test import Credentials, Topology


class RunnerTests(unittest.TestCase):
    @unittest.skipUnless(os.name == "nt", "Windows replacement sharing semantics")
    def test_atomic_report_survives_short_reader_and_preserves_old_on_timeout(self):
        with support.temporary_directory("atomic-report-") as directory:
            path = Path(directory) / "report.json"
            support.atomic_json(path, {"version": 1})
            reader = path.open("rb")
            release = threading.Timer(0.15, reader.close)
            release.start()
            try:
                support.atomic_json(path, {"version": 2})
            finally:
                release.join()
                reader.close()
            self.assertEqual(json.loads(path.read_text(encoding="utf-8")), {"version": 2})
            # 永久占用不可无限等待, 原文件仍须完整可读, 失败也不能截断原报告.
            with path.open("rb"), patch.object(support.time, "sleep") as pause:
                with self.assertRaises(PermissionError):
                    support.atomic_json(path, {"version": 3})
                self.assertEqual(pause.call_count, 40)
            self.assertEqual(json.loads(path.read_text(encoding="utf-8")), {"version": 2})

    def test_duration_and_scope(self):
        self.assertEqual(duration("2h"), 7200)
        self.assertEqual(duration("30m"), 1800)
        for value in ("0", "30s", "209s", "-1h", "25h", "inf", "2h;shutdown"):
            with self.assertRaises(Exception):
                duration(value)
        plan = arguments(["soak", "--duration", "210s", "--targets", "local", "--languages", "go,rust"])
        self.assertEqual(plan["duration"], 210)
        self.assertEqual(plan["languages"], ["go", "rust"])

    def test_configured_durations_keep_all_fault_types(self):
        from testkit.soak.soak_test import build_faults
        from testkit.catalog.soak_test import build_catalog_faults

        for schedule in (build_faults, build_catalog_faults):
            for seconds in (90, 600, 1800, 3600, 7200, 86400):
                faults = schedule(seconds)
                self.assertEqual({fault.kind for fault in faults}, {"script_flush", "kill_pubsub", "kill_normal", "pause", "restart"})
                self.assertTrue(all(0 < fault.at_seconds < seconds for fault in faults))

    def test_catalog_recovery_probes_do_not_retry_mutations(self):
        from testkit.catalog.sentinel_test import wait_command_connections

        first, second = MagicMock(), MagicMock()
        first.wait_line.side_effect = ["ERROR connection refused", "ROOT_READY"]
        second.wait_line.return_value = "ROOT_READY"
        wait_command_connections(first, second)
        self.assertEqual([call.args for call in first.send.call_args_list], [("PING",), ("PING",)])
        second.send.assert_called_once_with("PING")

    def test_fault_clock_waits_for_actual_workload_readiness(self):
        from testkit.soak.soak_test import Fault, FaultInjector, RedisMonitor

        injector = FaultInjector(MagicMock(), "redis://unused", time.monotonic() - 3600, [Fault(0, "script_flush")])
        with patch.object(injector, "_inject", return_value={}) as inject, contextlib.redirect_stdout(__import__("io").StringIO()):
            injector.observe("compiling and preparing peers\n")
            inject.assert_not_called()
            self.assertIsNone(injector._thread.ident)
            injector.observe("    soak_test.go:1: VERDANDI_SOAK_READY\n")
            injector._thread.join(timeout=5)
            injector.stop()
            inject.assert_called_once_with("script_flush")
            self.assertLess(injector.results[0]["started_seconds"], 5)
        # A failed preparation phase must still be safely cleanable.
        FaultInjector(MagicMock(), "redis://unused", 0, []).stop()
        RedisMonitor("redis://unused", 0, 5, None).stop()

    def test_failed_command_retains_partial_observations(self):
        observed = []
        with tempfile.TemporaryDirectory(dir=support.ROOT / "build") as temporary:
            with patch.dict(os.environ, {"VERDANDI_TEST_LOG_DIR": temporary}), contextlib.redirect_stdout(__import__("io").StringIO()):
                with self.assertRaises(RuntimeError):
                    support.run_command("partial", [sys.executable, "-u", "-c", "print('HEARTBEAT retained');raise SystemExit(3)"], on_output=observed.append)
        self.assertEqual(observed, ["HEARTBEAT retained\n"])

    def test_project_environment_does_not_mutate_parent(self):
        before = dict(os.environ)
        env = support.environment()
        self.assertEqual(dict(os.environ), before)
        for name in ("GOMODCACHE", "GOCACHE", "CARGO_HOME", "CARGO_TARGET_DIR", "NUGET_PACKAGES"):
            self.assertTrue(Path(env[name]).is_relative_to(support.ROOT / "build"))
        self.assertEqual(env["GOPROXY"], "off")
        self.assertEqual(env["CARGO_NET_OFFLINE"], "true")

    def test_readiness_failure_stops_every_started_peer(self):
        first, second = MagicMock(), MagicMock()
        first.__enter__.return_value = first
        second.__enter__.return_value = second
        second.wait_line.side_effect = TimeoutError("not ready")
        with (
            patch("testkit.catalog.interop_test.build_peers", return_value=("go", "rust")),
            patch("testkit.catalog.interop_test.Peer", side_effect=[first, second]),
        ):
            with self.assertRaises(TimeoutError):
                start_peers("Test", {}, {})
        first.__exit__.assert_called_once()
        second.__exit__.assert_called_once()

    def test_second_start_failure_stops_first_peer(self):
        first = MagicMock()
        first.__enter__.return_value = first
        with (
            patch("testkit.catalog.interop_test.build_peers", return_value=("go", "rust")),
            patch("testkit.catalog.interop_test.Peer", side_effect=[first, OSError("start")]),
        ):
            with self.assertRaises(OSError):
                start_peers("Test", {}, {})
        first.__exit__.assert_called_once()

    def test_process_timeout_stops_child(self):
        with tempfile.TemporaryDirectory(dir=support.ROOT / "build") as temporary:
            pid = Path(temporary) / "pid"
            command = [
                sys.executable,
                "-u",
                "-c",
                "import os,time,pathlib;pathlib.Path(__import__('sys').argv[1]).write_text(str(os.getpid()));time.sleep(60)",
                str(pid),
            ]
            with patch.dict(os.environ, {"VERDANDI_TEST_LOG_DIR": temporary}), contextlib.redirect_stdout(__import__("io").StringIO()):
                with self.assertRaises(TimeoutError):
                    support.run_command("timeout", command, timeout=1)
            self.assertTrue(pid.exists())
            if os.name != "nt":
                with self.assertRaises(ProcessLookupError):
                    os.kill(int(pid.read_text()), 0)

    def test_command_preserves_arguments_and_required_result(self):
        with tempfile.TemporaryDirectory(dir=support.ROOT / "build") as temporary:
            values = ["", "two words", 'a"b', "中文", "$(literal)"]
            command = [sys.executable, "-c", "import sys,json;print('RESULT '+json.dumps(sys.argv[1:],ensure_ascii=False))", *values]
            with patch.dict(os.environ, {"VERDANDI_TEST_LOG_DIR": temporary}), contextlib.redirect_stdout(__import__("io").StringIO()):
                result = support.run_command("arguments", command, required_output="RESULT ", timeout=10)
            parsed = json.loads(result["output"].split("RESULT ", 1)[1])
            self.assertEqual(parsed, values)

    def test_successful_parent_cannot_leave_running_grandchild(self):
        with tempfile.TemporaryDirectory(dir=support.ROOT / "build") as temporary:
            command = [
                sys.executable,
                "-u",
                "-c",
                "import subprocess,sys; p=subprocess.Popen([sys.executable,'-c','import time;time.sleep(60)']); print('CHILD',p.pid,flush=True)",
            ]
            with patch.dict(os.environ, {"VERDANDI_TEST_LOG_DIR": temporary}), contextlib.redirect_stdout(__import__("io").StringIO()):
                started = time.monotonic()
                result = support.run_command("orphan", command, timeout=10)
            self.assertLess(time.monotonic() - started, 5)
            self.assertIn("CHILD", result["output"])

    def test_existing_interop_zone_is_never_deleted(self):
        from testkit.catalog import interop_test as catalog
        from testkit.interop import interop_test as registration

        for module, lookup in ((catalog, "catalog_keys"), (registration, "zone_keys")):
            client = MagicMock()
            with (
                patch.object(module.redis.Redis, "from_url", return_value=client),
                patch.object(module, lookup, return_value=[b"foreign"]),
                patch.object(sys, "argv", ["test", "--redis-url", "redis://localhost"]),
            ):
                with self.assertRaisesRegex(RuntimeError, "already exists"):
                    module.main()
            client.unlink.assert_not_called()
            client.delete.assert_not_called()
            client.close.assert_called_once()

    def test_clean_manifest_is_idempotent(self):
        with tempfile.TemporaryDirectory() as temporary, patch.object(resources, "ROOT", Path(temporary)):
            remote = MagicMock(host="test", username="user", project="/project")
            owner = resources.Resources(remote, "aabbccdd")
            owner.cleanup()
            owner.cleanup()
            remote.run.assert_not_called()

    def test_foreign_container_survives_and_cleanup_fails(self):
        with tempfile.TemporaryDirectory() as temporary, patch.object(resources, "ROOT", Path(temporary)):
            remote = MagicMock(host="test", username="user", project="/project")
            remote.run.side_effect = ["verdandi-test-aabbccdd\n", json.dumps([{"Id": "1" * 64, "Config": {"Labels": {"verdandi.test": "foreign"}}}])]
            owner = resources.Resources(remote, "aabbccdd")
            owner.container("verdandi-test-aabbccdd")
            with self.assertRaisesRegex(RuntimeError, "foreign container"):
                owner.cleanup()
            self.assertFalse(any("rm " in call.args[0] for call in remote.run.call_args_list))
            self.assertEqual(json.loads(owner.path.read_text())["status"], "cleanup_failed")

    def test_container_removal_uses_verified_id(self):
        with tempfile.TemporaryDirectory() as temporary, patch.object(resources, "ROOT", Path(temporary)):
            remote = MagicMock(host="test", username="user", project="/project")
            remote.run.side_effect = ["verdandi-test-aabbccdd\n", json.dumps([{"Id": "1" * 64, "Config": {"Labels": {"verdandi.test": "aabbccdd"}}}]), ""]
            owner = resources.Resources(remote, "aabbccdd")
            owner.container("verdandi-test-aabbccdd")
            owner.cleanup()
            self.assertEqual(remote.run.call_args.args[0], "docker rm -f " + "1" * 64)

    def test_sentinel_collision_never_registers_foreign_resources(self):
        with tempfile.TemporaryDirectory() as temporary, patch.object(resources, "ROOT", Path(temporary)):
            remote = MagicMock(host="test", username="user", project="/project")
            remote.run.return_value = "verdandi-it-aabbccdd-redis-1\n"
            topology = Topology(remote, "aabbccdd", Credentials.generate())
            with patch.object(topology, "_assert_ports_free"):
                with self.assertRaisesRegex(Exception, "collision"):
                    topology.deploy()
            topology.cleanup()
            self.assertEqual(remote.run.call_count, 1)

    def test_cleanup_recovers_failed_creation_intent(self):
        with tempfile.TemporaryDirectory() as temporary, patch.object(resources, "ROOT", Path(temporary)):
            remote = MagicMock(host="test", username="user", project="/project")
            owner = resources.Resources(remote, "aabbccdd")
            owner.container("verdandi-test-aabbccdd")
            owner.lock.close()  # Simulated dead creator; the OS releases this on death.
            remote.run.return_value = ""
            resources.Resources.recover(remote)
            self.assertEqual(json.loads(owner.path.read_text())["status"], "cleaned")

    def test_live_resource_manifest_is_not_recovered(self):
        with tempfile.TemporaryDirectory() as temporary, patch.object(resources, "ROOT", Path(temporary)):
            remote = MagicMock(host="test", username="user", project="/project")
            owner = resources.Resources(remote, "aabbccdd")
            try:
                owner.container("verdandi-test-aabbccdd")
                resources.Resources.recover(remote)
                remote.run.assert_not_called()
            finally:
                owner.lock.close()

    def test_directory_scope_rejects_parent_traversal(self):
        with tempfile.TemporaryDirectory() as temporary, patch.object(resources, "ROOT", Path(temporary)):
            remote = MagicMock(host="test", username="user", project="/project")
            owner = resources.Resources(remote, "aabbccdd")
            try:
                for path in ("/", "/project", "/tmp/foreign", "/project/build/testkit/fixtures/../soak-aabbccdd"):
                    with self.assertRaises(ValueError):
                        owner.directory(path)
                remote.run.assert_not_called()
            finally:
                owner.cleanup()

    def test_temporary_cleanup_and_crash_recovery(self):
        with tempfile.TemporaryDirectory(dir=support.ROOT / "build") as directory, patch.object(support, "ROOT", Path(directory)):
            with support.temporary_directory() as temporary:
                path = Path(temporary)
                (path / "nested").mkdir()
                (path / "nested/data").write_bytes(b"owned")
                support.recover_temporary()
                self.assertTrue(path.exists())
            self.assertFalse(path.exists())
            path.mkdir()
            (path / ".owner").write_text(path.name)
            (path / "orphan").write_bytes(b"owned")
            support.recover_temporary()
            self.assertFalse(path.exists())

    def test_temporary_foreign_owner_is_preserved(self):
        with tempfile.TemporaryDirectory(dir=support.ROOT / "build") as directory, patch.object(support, "ROOT", Path(directory)):
            path = Path(directory) / "build/testkit/tmp/verdandi-0123456789abcdef"
            path.mkdir(parents=True)
            (path / ".owner").write_text("foreign")
            with self.assertRaisesRegex(ValueError, "ownership mismatch"):
                support.recover_temporary()
            self.assertTrue(path.exists())

    def test_failed_or_interrupted_campaign_always_reports_cleanup(self):
        import signal
        from testkit import run

        for failure, status in ((RuntimeError("test failure"), "blocked"), (KeyboardInterrupt(), "interrupted")):
            owned_scratch = []

            def fail_after_temporary_file(*args):
                scratch = Path(os.environ["TMPDIR"])
                self.assertTrue(scratch.is_relative_to(support.ROOT / "build/testkit/tmp"))
                for name in ("TEMP", "TMP", "GOTMPDIR"):
                    self.assertEqual(os.environ[name], str(scratch))
                (scratch / "sdk-checkpoint.sqlite").write_bytes(b"test")
                owned_scratch.append(scratch)
                raise failure

            with (
                tempfile.TemporaryDirectory(dir=support.ROOT / "build") as temporary,
                patch.object(run, "ROOT", Path(temporary)),
                patch.object(support, "available_memory", return_value=8 * 1024 * 1024 * 1024),
                patch.dict(os.environ),
                patch("testkit.campaign.source_manifest", return_value={}),
                patch("testkit.suites.execute", side_effect=fail_after_temporary_file),
                patch.object(resources, "Remote"),
                patch.object(resources.Resources, "recover") as cleanup,
                contextlib.redirect_stdout(__import__("io").StringIO()),
            ):
                previous = {sig: signal.getsignal(sig) for sig in (signal.SIGINT, signal.SIGTERM)}
                try:
                    options = vars(
                        __import__("argparse").Namespace(
                            mode="regression",
                            targets=["local"],
                            languages=["go"],
                            password="private",
                            host="test",
                            user="test",
                            remote_project="/project",
                            preflight_only=False,
                        )
                    )
                    self.assertNotEqual(run.execute(options), 0)
                    report = json.loads((Path(options["output"]) / "report.json").read_text())
                    self.assertEqual(report["status"], status)
                    self.assertEqual(report["cleanup"], "cleaned")
                    self.assertNotIn("private", json.dumps(report))
                    cleanup.assert_called_once()
                    self.assertEqual(len(owned_scratch), 1)
                    self.assertFalse(owned_scratch[0].exists())
                finally:
                    for sig, handler in previous.items():
                        signal.signal(sig, handler)

    def test_remote_failure_does_not_claim_remote_cleanup(self):
        import signal
        from testkit import run

        with (
            tempfile.TemporaryDirectory(dir=support.ROOT / "build") as temporary,
            patch.object(run, "ROOT", Path(temporary)),
            patch.object(support, "available_memory", return_value=8 * 1024 * 1024 * 1024),
            patch.dict(os.environ),
            patch("testkit.campaign.source_manifest", return_value={}),
            patch("testkit.campaign.remote_campaign", side_effect=TimeoutError("SSH lost")),
            patch.object(resources, "Remote"),
            patch.object(resources.Resources, "recover"),
            contextlib.redirect_stdout(__import__("io").StringIO()),
        ):
            previous = {sig: signal.getsignal(sig) for sig in (signal.SIGINT, signal.SIGTERM)}
            try:
                options = dict(
                    mode="regression",
                    targets=["linux"],
                    languages=["go"],
                    password="private",
                    host="test",
                    user="test",
                    remote_project="/project",
                    preflight_only=False,
                )
                self.assertNotEqual(run.execute(options), 0)
                report = json.loads((Path(options["output"]) / "report.json").read_text())
                self.assertEqual(report["cleanup"], "pending")
            finally:
                for sig, handler in previous.items():
                    signal.signal(sig, handler)


if __name__ == "__main__":
    unittest.main()
