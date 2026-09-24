"""服务脚本参数与失败传播, 隔离替身不启动真实构建或部署."""

from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(os.name == "posix" and shutil.which("bash"), "Linux shell entrypoints")
class ServiceEntrypoints(unittest.TestCase):
    def invoke(self, options, status=0, script="test-services.sh"):
        """独立目录中的构建替身只打印收到的参数与指定退出码."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "scripts").mkdir()
            (root / "astra").mkdir()
            target = root / "scripts" / script
            shutil.copyfile(ROOT / "scripts" / script, target)
            (root / "astra/build.sh").write_text(
                f"#!/bin/bash\nprintf 'BUILD:%s\\n' \"$@\"\nexit {status}\n",
                encoding="utf-8",
            )
            return subprocess.run(["bash", str(target), *options], capture_output=True, text=True, timeout=10)

    def test_regression_forwards_arguments(self):
        result = self.invoke(["--mode=regression", "--jobs=2", "--test-jobs=3"])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("BUILD:regression", result.stdout)
        self.assertIn("BUILD:--jobs=2", result.stdout)
        self.assertIn("BUILD:--test-jobs=3", result.stdout)

    def test_skip_checks_still_runs_tests(self):
        result = self.invoke(["--skip-checks"])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("BUILD:test", result.stdout)

    def test_failure_propagates(self):
        result = self.invoke([], status=37)
        self.assertEqual(result.returncode, 37)
        self.assertNotIn("passed", result.stdout.lower())

    def test_retired_and_unknown_options_are_rejected_before_build(self):
        for options in (["--mode=soak"], ["--mode=scale"], ["--duration=5"], ["--unknown"]):
            with self.subTest(options=options):
                result = self.invoke(options)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertNotIn("BUILD:", result.stdout)

    def test_removed_checks_do_not_silently_pass(self):
        for option in ("--race", "--fuzz-seconds=1", "--service=supervisor", "--jobs=257"):
            with self.subTest(option=option):
                result = self.invoke([option], script="check-services.sh")
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertNotIn("passed", result.stdout.lower())


if __name__ == "__main__":
    unittest.main()
