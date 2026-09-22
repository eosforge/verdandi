"""Offline entry checks, including real CMake cache recovery and empty CTest rejection."""

from __future__ import annotations

import contextlib
import io
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import build


class BuildEntryTests(unittest.TestCase):
    def test_invalid_operation_combinations(self):
        for arguments in (
            ["regression", "--core-only"],
            ["soak", "--core-only"],
            ["scale", "--core-only"],
            ["benchmark", "--core-only"],
            ["build", "--core-only", "--benchmarks"],
            ["build", "--core-only", "--measure-allocations", "--profile", "release"],
            ["scale", "--measure-allocations"],
            ["benchmark", "--profile", "tsan"],
            ["benchmark", "--profile", "release"],
            ["benchmark", "--profile", "tsan", "--benchmark-smoke"],
            ["build", "--benchmark-smoke"],
            ["soak", "--duration", "0"],
            ["soak", "--duration", "604801"],
            ["build", "--jobs", "-1"],
            ["test", "--test-jobs", "257"],
        ):
            with self.subTest(arguments=arguments), contextlib.redirect_stderr(
                io.StringIO()
            ), self.assertRaises(SystemExit) as error:
                build.parse_options(arguments)
            self.assertEqual(error.exception.code, 2)

    def test_supported_profiles_and_operations(self):
        for arguments in (
            ["build"],
            ["test", "--core-only"],
            ["regression", "--profile", "asan"],
            ["soak", "--duration", "60", "--profile", "release"],
            ["scale", "--measure-allocations", "--profile", "release"],
            ["check-generated"],
        ):
            with self.subTest(arguments=arguments):
                self.assertEqual(build.parse_options(arguments).command, arguments[0])

    def temporary_project(self):
        temporary_root = build.ROOT / "build/tmp"
        temporary_root.mkdir(parents=True, exist_ok=True)
        directory = tempfile.TemporaryDirectory(
            prefix="astra-build-test-", dir=temporary_root
        )
        project = Path(directory.name).resolve()
        self.assertTrue(project.is_relative_to(temporary_root.resolve()))
        self.addCleanup(directory.cleanup)
        return project

    def test_environment_is_child_only_and_avoids_empty_search_paths(self):
        project = self.temporary_project()
        inherited = {
            "PATH": "parent-bin",
            "LD_LIBRARY_PATH": "",
            "CMAKE_BUILD_PARALLEL_LEVEL": "8",
        }
        with patch.object(build, "ROOT", project), patch.dict(
            os.environ, inherited, clear=True
        ):
            actual = build.environment(project / "deps")
            self.assertEqual(dict(os.environ), inherited)
        self.assertEqual(
            actual["LD_LIBRARY_PATH"], str(project / "build/tools/gcc-16.2.0/lib64")
        )
        self.assertNotIn(str(project / "deps/lib"), actual["LD_LIBRARY_PATH"])
        self.assertEqual(actual["CMAKE_BUILD_PARALLEL_LEVEL"], "1")
        self.assertEqual(actual["PATH"].split(os.pathsep)[-1], "parent-bin")

    def test_explicit_parent_runtime_path_is_preserved(self):
        project = self.temporary_project()
        with patch.object(build, "ROOT", project), patch.dict(
            os.environ, {"LD_LIBRARY_PATH": "parent-lib"}, clear=True
        ):
            actual = build.environment(project / "deps")
        self.assertEqual(actual["LD_LIBRARY_PATH"].split(os.pathsep)[-1], "parent-lib")

    def test_budget_respects_current_memory_and_explicit_ceilings(self):
        self.assertEqual(build.parallel(16, 8 << 30, "debug"), (8, 16))
        self.assertEqual(build.parallel(16, 1536 << 20, "debug"), (1, 4))
        self.assertEqual(build.parallel(16, 8 << 30, "asan"), (5, 15))
        self.assertEqual(build.parallel(16, 8 << 30, "tsan", 2, 3), (2, 3))
        self.assertEqual(build.parallel(16, 1536 << 20, "asan", 16, 16), (1, 2))
        self.assertEqual(build.parallel(2, 8 << 30, "debug"), (2, 2))
        self.assertEqual(build.parallel(16, None, "debug"), (1, 1))
        self.assertEqual(build.parallel(16, 0, "debug"), (1, 1))

    def test_go_caches_and_download_controls_are_child_only(self):
        project = self.temporary_project()
        inherited = {
            "GOPATH": "global",
            "GOPROXY": "https://example.invalid",
            "GOFLAGS": "-mod=mod",
            "GOTOOLCHAIN": "auto",
        }
        with patch.object(build, "ROOT", project), patch.dict(
            os.environ, inherited, clear=True
        ):
            actual = build.environment(project / "deps", 3)
            self.assertEqual(dict(os.environ), inherited)
        self.assertEqual(actual["GOPATH"], str(project / "build/deps/go"))
        self.assertEqual(actual["GOMODCACHE"], str(project / "build/deps/go/pkg/mod"))
        self.assertEqual(actual["GOCACHE"], str(project / "build/cache/go"))
        self.assertEqual(actual["GOPROXY"], "off")
        self.assertEqual(actual["GOSUMDB"], "off")
        self.assertEqual(actual["GOTOOLCHAIN"], "local")
        self.assertEqual(actual["GOWORK"], "off")
        self.assertEqual(actual["GOFLAGS"], "-mod=readonly")
        self.assertEqual(actual["CMAKE_BUILD_PARALLEL_LEVEL"], "3")
        self.assertEqual(actual["GOMAXPROCS"], "3")

    def run_fixture(self, *, no_tests=False):
        """使用真实 CMake/CTest 的无语言夹具, 验证入口行为; 不模拟 CTest 返回值或编译项目源码."""
        if not shutil.which("cmake") or not shutil.which("ctest"):
            self.fail("Build entry tests require the already installed CMake and CTest")
        project = self.temporary_project()
        source = project / "astra"
        output = project / "build/astra/core-debug"
        source.mkdir()
        (source / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.28)\n"
            "project(build_entry_fixture LANGUAGES NONE)\n"
            "include(CTest)\n"
            "if(BUILD_TESTING AND NOT FIXTURE_NO_TESTS)\n"
            '  add_test(NAME executed COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_CURRENT_BINARY_DIR}/executed")\n'
            "endif()\n",
            encoding="utf-8",
        )
        # 无语言夹具不会调用编译器. 这个存在性标记只满足入口的离线工具检查.
        compiler = project / "build/tools/gcc-16.2.0/bin/g++"
        compiler.parent.mkdir(parents=True)
        compiler.touch()

        def execute(command, env):
            # Windows 的既有 CMake 默认使用 Visual Studio 多配置生成器, 仅夹具补选其 Debug 测试配置.
            # 正式服务仍只支持 Linux, 这个适配不会进入 build.py.
            command = list(map(str, command))
            if os.name == "nt" and command[0] == "ctest":
                command += ["-C", "Debug"]
            return subprocess.run(
                command,
                cwd=project,
                env=env,
                check=True,
                capture_output=True,
                text=True,
                encoding="utf-8",
            )

        execute(
            [
                "cmake",
                "-S",
                source,
                "-B",
                output,
                "-DBUILD_TESTING=OFF",
                "-DFIXTURE_NO_TESTS=" + ("ON" if no_tests else "OFF"),
            ],
            dict(os.environ),
        )
        # 在两端测试同一个 Linux 入口策略. 此夹具只运行跨平台的 cmake -E, 不冒充 Windows 服务构建.
        with (
            patch.object(build, "ROOT", project),
            patch.object(build, "SOURCE", source),
            patch.object(build, "PREFIX", project / "deps"),
            patch.object(build, "run", execute),
            patch.object(sys, "platform", "linux"),
            patch.object(sys, "argv", ["build.py", "test", "--core-only"]),
        ):
            build.main()
        return output

    def test_cached_disabled_tests_are_reenabled_and_executed(self):
        output = self.run_fixture()
        self.assertTrue((output / "executed").is_file())
        self.assertIn(
            "BUILD_TESTING:BOOL=ON",
            (output / "CMakeCache.txt").read_text(encoding="utf-8"),
        )

    def test_empty_suite_fails_the_entry(self):
        with self.assertRaises(subprocess.CalledProcessError) as error:
            self.run_fixture(no_tests=True)
        self.assertEqual(Path(error.exception.cmd[0]).stem, "ctest")
        self.assertIn(
            "No tests were found", error.exception.stderr + error.exception.stdout
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
