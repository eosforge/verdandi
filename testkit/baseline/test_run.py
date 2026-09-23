"""统一基线运行器的无服务单元测试, 重点覆盖失败路径资源回收."""

import importlib.util
import json
from pathlib import Path
import subprocess
from types import SimpleNamespace
import unittest
from unittest.mock import MagicMock, patch

spec = importlib.util.spec_from_file_location("baseline", Path(__file__).with_name("run.py"))
baseline = importlib.util.module_from_spec(spec)
spec.loader.exec_module(baseline)
report_spec = importlib.util.spec_from_file_location("report", Path(__file__).with_name("report.py"))
report = importlib.util.module_from_spec(report_spec)
report_spec.loader.exec_module(report)


class RunnerTest(unittest.TestCase):
    def test_arguments(self):
        """新旧适配器只能在程序和端点上不同, 参数数目及版本契约一致."""
        case = json.loads(Path(__file__).with_name("cases.json").read_text())[0]
        first = baseline.command("comet", "127.0.0.1:1", case)
        second = baseline.command("redis", "127.0.0.1:2", case)
        self.assertEqual(len(first), 16)
        self.assertEqual(first[2:], second[2:])
        self.assertEqual(first[-1], "v1")
        self.assertEqual(baseline.command("redis", "127.0.0.1:2", dict(case, legacy_view_ms=10))[-2:], ["v1", "10"])

    def test_external_requires_owner(self):
        """提供地址却缺少所有者 PID 时拒绝, 不接触数据库."""
        arguments = SimpleNamespace(redis="127.0.0.1:1", redis_pid=None)
        with self.assertRaises(ValueError), baseline.server(arguments):
            self.fail("Missing owner accepted")

    def test_standard_topology(self):
        """默认三节点, 单/双节点仅允许显式诊断, 不静默修改客户端和负载数量."""
        case = dict(clients=6, writers=6, records=96, fanout=3)
        self.assertEqual(baseline.topology(case)["stars"], 3)
        for count in (1, 2):
            with self.subTest(stars=count), self.assertRaises(ValueError):
                baseline.topology(dict(case, stars=count))
            self.assertEqual(baseline.topology(dict(case, stars=count, diagnostic=True))["stars"], count)
        for changes in (dict(clients=1), dict(clients=4), dict(writers=2), dict(writers=4), dict(fanout=2), dict(records=95), dict(stars=0), dict(stars=True)):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                baseline.topology(dict(case, **changes))

    def test_comet_routes_every_endpoint(self):
        """夹具必须拉起三台 Star, 探针拿到全部地址而非仅第一台; 不实际启动服务."""
        case = baseline.topology(json.loads(Path(__file__).with_name("cases.json").read_text())[0])
        arguments = SimpleNamespace(binaries=Path("binaries"))
        with patch.object(baseline.measure, "cluster", return_value=[]) as cluster:
            self.assertEqual(baseline.comet(arguments, case, Path("output")), [])
        self.assertEqual(cluster.call_args.args[1]["stars"], 3)
        command = cluster.call_args.args[3](["127.0.0.1:1", "127.0.0.1:2", "127.0.0.1:3"])
        self.assertEqual(command[1], "127.0.0.1:1,127.0.0.1:2,127.0.0.1:3")

    def test_standard_matrix(self):
        """标准矩阵全部覆盖三节点, 单 Scope 合并与多 Scope 并行都须存在."""
        cases = [baseline.topology(case) for case in json.loads(Path(__file__).with_name("cases.json").read_text())]
        self.assertTrue(all(case["stars"] >= 3 and not case.get("diagnostic", False) for case in cases))
        for domain in ("catalog", "ephemeris"):
            self.assertTrue(any(case["domain"] == domain and case["groups"] == 1 for case in cases))
            self.assertTrue(any(case["domain"] == domain and case["groups"] >= 3 for case in cases))
            for clients in (3, 6):
                self.assertTrue(
                    any(case["domain"] == domain and case["writers"] == 24 and case["clients"] == clients and case["mode"] == "receipt" for case in cases)
                )

    def test_external_lifecycle(self):
        """外层所有者模式不触碰 Docker, 由外层负责清理."""
        arguments = SimpleNamespace(redis="127.0.0.1:1", redis_pid=100)
        with patch.object(baseline.subprocess, "run") as execute:
            with baseline.server(arguments):
                pass
            execute.assert_not_called()

    def exercise(self, timeout):
        """模拟正常启动后异常, 或 run 超时但容器已经创建; 两者都必须删除确切 ID."""
        arguments = SimpleNamespace(redis=None, redis_pid=None, sudo=False)
        name = "astra-baseline-aaaaaaaaaaaa"
        identity = "owned-container-id"
        calls = []

        def execute(command, **kwargs):
            calls.append(command)
            if command[1] == "run" and timeout:
                raise subprocess.TimeoutExpired(command, 30)
            if command[1] == "inspect":
                state = [dict(Id=identity, Config=dict(Labels={"astra.baseline": name}))]
                return subprocess.CompletedProcess(command, 0, json.dumps(state), "")
            return subprocess.CompletedProcess(command, 0, "", "")

        connection = MagicMock()
        connection.__enter__.return_value.recv.return_value = b"+PONG\r\n"
        with (
            patch.object(baseline.uuid, "uuid4", return_value=SimpleNamespace(hex="a" * 32)),
            patch.object(baseline.measure, "port", return_value=12345),
            patch.object(baseline.socket, "create_connection", return_value=connection),
            patch.object(baseline.subprocess, "run", side_effect=execute),
            patch.object(baseline.subprocess, "check_output", return_value="100"),
        ):
            expected = subprocess.TimeoutExpired if timeout else RuntimeError
            with self.assertRaises(expected), baseline.server(arguments):
                raise RuntimeError("Injected workload failure")
        self.assertIn("--pull=never", calls[0])
        self.assertEqual(calls[-1], ["docker", "rm", "--force", identity])

    def test_failure_cleanup(self):
        self.exercise(False)

    def test_ambiguous_start_cleanup(self):
        self.exercise(True)

    def test_paired_statistics(self):
        """一侧换页时整对排除, 不用另一侧的稳定结果构造虚假比较."""
        rows = []
        for repeat in range(2):
            for implementation in ("redis", "comet"):
                metric = dict(metric="commit", operations_per_second=100, p50_us=1, p95_us=2, p99_us=3, p999_us=4)
                rows.append(
                    dict(
                        case=dict(mode="receipt"),
                        round=repeat,
                        implementation=implementation,
                        passed=True,
                        stable_memory=not (repeat == 1 and implementation == "redis"),
                        measurements=[metric],
                    )
                )
        result = report.summarize(rows)
        self.assertEqual(result[0]["paired_rounds"], 1)
        self.assertEqual(result[0]["comet_redis_throughput_ratio"], 1)
        with self.assertRaises(ValueError):
            report.summarize(rows + rows[:1])


if __name__ == "__main__":
    unittest.main()
