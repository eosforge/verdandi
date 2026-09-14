"""服务测试器自身的输入隔离、结果判定与清理边界."""

import json
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from testkit.services import Host, ServiceProcess, star_binary_directory
from testkit.support import ROOT


class ServiceHarnessTests(unittest.TestCase):
    def test_implementation_selection_rejects_unknown_paths(self):
        self.assertEqual(
            star_binary_directory("cpp"), ROOT / "build/cluster-cpp/release"
        )
        with self.assertRaises(ValueError):
            star_binary_directory("rust")
        with self.assertRaises(ValueError):
            star_binary_directory("../arbitrary")

    def test_sanitizer_failure_is_rejected_even_during_forced_cleanup(self):
        host = Host()
        self.addCleanup(host.close)
        directory = host.directory
        process = ServiceProcess(
            [
                sys.executable,
                "-B",
                "-c",
                "import sys; print('WARNING: ThreadSanitizer: data race', flush=True); sys.exit(66)",
            ],
            directory / "sanitizer.log",
        )
        host.processes["diagnostic"] = process
        process.process.wait(timeout=10)
        with self.assertRaises(ExceptionGroup):
            host.close()
        self.assertFalse(process.reader.is_alive())
        self.assertFalse(directory.exists())

    def test_graceful_stop_rejects_already_failed_process(self):
        host = Host()
        process = ServiceProcess(
            [sys.executable, "-B", "-c", "raise SystemExit(66)"],
            host.directory / "early-exit.log",
        )
        host.processes["early-exit"] = process
        try:
            process.process.wait(timeout=10)
            with self.assertRaisesRegex(RuntimeError, "returned 66"):
                process.stop(True)
            self.assertFalse(process.reader.is_alive())
        finally:
            host.close()

    def test_bad_login_uses_owned_copy_and_keeps_process_name(self):
        original = ROOT / "cluster/tests/fixtures/star-a/login.json"
        before = original.read_bytes()
        host = Host()
        directory = host.directory
        try:
            with patch("testkit.services.ServiceProcess") as process:
                process.return_value.process.pid = 123
                host.call(
                    "start",
                    name="bad-login",
                    kind="star",
                    role="star-a",
                    login_case="wrong-password",
                    address="127.0.0.1:7443",
                    supervisor="127.0.0.1:7442",
                )
                self.assertEqual(set(host.processes), {"bad-login"})
                command = process.call_args.args[0]
                copied = Path(
                    next(
                        value.split("=", 1)[1]
                        for value in command
                        if isinstance(value, str) and value.startswith("--identity=")
                    )
                )
                self.assertTrue(copied.is_relative_to(directory))
                self.assertEqual(
                    json.loads((copied / "login.json").read_text())["password"],
                    "wrong-public-test-password",
                )
                host.call("stop", name="bad-login", graceful=False)
                process.return_value.stop.assert_called_once_with(False)
                self.assertEqual(original.read_bytes(), before)
        finally:
            host.close()
        self.assertFalse(directory.exists())

    def test_unknown_login_case_does_not_launch_or_copy_credentials(self):
        host = Host()
        before = list(host.directory.iterdir())
        try:
            with patch("testkit.services.ServiceProcess") as process:
                with self.assertRaises(ValueError):
                    host.call(
                        "start",
                        name="bad",
                        kind="star",
                        role="star-a",
                        login_case="arbitrary",
                    )
                process.assert_not_called()
                self.assertEqual(list(host.directory.iterdir()), before)
        finally:
            host.close()

    def test_cli_nonzero_success_claim_is_rejected(self):
        host = Host()
        try:
            with patch(
                "testkit.services.subprocess.run",
                return_value=SimpleNamespace(returncode=1, stdout="version", stderr=""),
            ):
                with self.assertRaises(AssertionError):
                    host.call("verify_cli")
        finally:
            host.close()

    def test_cli_old_identity_instructions_are_rejected(self):
        host = Host()
        try:
            outputs = [
                SimpleNamespace(returncode=0, stdout="star 0.1.0", stderr=""),
                SimpleNamespace(
                    returncode=0, stdout="Process id is issued by Supervisor", stderr=""
                ),
            ]
            with patch("testkit.services.subprocess.run", side_effect=outputs):
                with self.assertRaises(AssertionError):
                    host.call("verify_cli")
        finally:
            host.close()


if __name__ == "__main__":
    unittest.main()
