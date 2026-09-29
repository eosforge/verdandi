"""开发门禁的拒绝路径与采集生命周期; 合成记录只作为测试夹具, 不作为组件验收证据."""

import copy
import datetime as dt
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from tools import verify as v

ROOT = Path(__file__).resolve().parents[1]


def fixture():
    config = v.decode(v.read(ROOT / "development.json"))
    component = config["components"][0]
    component["level"] = "L1"
    component["role"] = "component"
    component["evidence_trust"] = "local"
    component["convergence"] = {"metric": "fixture", "baseline": 1, "target": 0, "deadline": "2099-01-01T00:00:00Z", "approval_ref": "fixture-only"}
    component["requirements"] = [component["requirements"][0]]
    config["checks"] = [config["checks"][0]]
    check = config["checks"][0]
    check["expected_tests"] = ["TestVerification/atomic"]
    check["probes"] = [check["probes"][0]]
    return config


def events(check, hits=7):
    package = check["package"]
    test = check["expected_tests"][0]
    return [
        {"Package": package, "Action": "start"},
        {"Package": package, "Action": "run", "Test": test},
        {
            "Package": package,
            "Action": "output",
            "Test": test,
            "Output": '    verification_test.go:19: ASTRA-CONTRACT {"contract":"STORAGE-ATOMIC","hits":' + str(hits) + "}\n",
        },
        {"Package": package, "Action": "pass", "Test": test},
        {"Package": package, "Action": "pass"},
    ]


def stream(entries):
    return b"".join((json.dumps(entry) + "\n").encode() for entry in entries)


class ConfigurationTests(unittest.TestCase):
    def test_strict_json(self):
        for value, reason in [
            (b'{"a":1,"a":2}', "duplicate-key"),
            (b"NaN", "non-finite"),
            (b"1e999", "non-finite"),
            (b'{"a":', "json-invalid"),
            (b"[" * 40 + b"0" + b"]" * 40, "json-depth"),
        ]:
            with self.subTest(value=value), self.assertRaisesRegex(v.Invalid, reason):
                v.decode(value)

    def test_json_size(self):
        with self.assertRaisesRegex(v.Invalid, "json-size"):
            v.decode(b" " * (v.LIMIT + 1))

    def test_path_boundary(self):
        with tempfile.TemporaryDirectory() as directory:
            for name in ["../x", "/x", "a/../x", "a//b", "C:/x", "a\\b", "."]:
                with self.subTest(name=name), self.assertRaises(v.Invalid):
                    v.inside(directory, name)
            self.assertEqual(v.inside(directory, "safe/file"), Path(directory) / "safe/file")

    def test_symlink_boundary(self):
        if platform.system() != "Linux":
            self.skipTest("Linux symlink fixture")
        with tempfile.TemporaryDirectory() as directory:
            (Path(directory) / "link").symlink_to("/tmp", target_is_directory=True)
            with self.assertRaisesRegex(v.Invalid, "path-symlink"):
                v.inside(directory, "link/file")

    def test_real_configuration(self):
        config, registry, pins = v.configuration(ROOT / "development.json")
        self.assertGreater(len(v.expand(config)), 4)
        self.assertEqual(len(registry["rules"]), 29)
        self.assertEqual(len(pins["policy"]), 64)
        self.assertEqual(config["components"][0]["level"], "L3")

    def test_closed_shapes_and_semantics(self):
        original = v.decode(v.read(ROOT / "development.json"))
        mutations = {
            "unknown-field": lambda c: c.update(typo=True),
            "missing-required": lambda c: c.pop("components"),
            "ungraded": lambda c: c["components"][0].pop("level"),
            "unknown-rule": lambda c: c["components"][0]["requirements"][0].update(rule="DEV-FAKE"),
            "duplicate-check": lambda c: c["checks"].append(c["checks"][0]),
            "missing-matrix": lambda c: c["components"][0].update(platforms=[]),
            "empty-obligations": lambda c: c["components"][0].update(requirements=[]),
            "unknown-platform": lambda c: c["components"][0].update(platforms=["other"]),
            "downgrade": lambda c: c["components"][0].update(level="L1"),
            "missing-l3": lambda c: c["components"][0].update(requirements=c["components"][0]["requirements"][:-6]),
            "unknown-contract": lambda c: c["components"][0]["requirements"][0].update(contract="MISSING"),
            "unobservable": lambda c: c["checks"][0]["probes"].pop(),
            "online": lambda c: c["checks"][0]["environment"].update(GOPROXY="direct"),
            "cached-test": lambda c: c["checks"][0]["argv"].remove("-count=1"),
        }
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "development.json"
            for name, mutate in mutations.items():
                with self.subTest(name=name):
                    config = copy.deepcopy(original)
                    mutate(config)
                    path.write_bytes(v.encode(config))
                    with self.assertRaises(v.Invalid):
                        v.configuration(path)

    def test_registry_and_closure(self):
        registry = v.decode(v.read(ROOT / "tools/rules.json"))
        self.assertIn("DEV-CONTRACT", v.closure(registry, ["DEV-ASYNC"]))
        with self.assertRaisesRegex(v.Invalid, "unknown-rule"):
            v.closure(registry, ["DEV-INVENTED"])
        registry["rules"][0]["depends_on"] = [registry["rules"][0]["id"]]
        with self.assertRaisesRegex(v.Invalid, "rule-cycle"):
            v.closure(registry, [registry["rules"][0]["id"]])

    def test_schema_not_template(self):
        with self.assertRaises(v.Invalid):
            v.schema({"schema_version": 2, "run_id": "TEMPLATE-NOT-EXECUTED", "gate": {"status": "eligible"}}, "evidence")

    def test_frozen_output_not_reused(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "fact.json"
            v.write_new(path, {"first": "failure"})
            with self.assertRaises(FileExistsError):
                v.write_new(path, {"second": "passed"})
            self.assertEqual(v.decode(v.read(path)), {"first": "failure"})


class AuthorityTests(unittest.TestCase):
    def test_external_pin_expiry_revocation_and_schema(self):
        config = v.decode(v.read(ROOT / "development.json"))
        pins = {"policy": "a" * 64, "rules": "b" * 64}
        record = {
            "schema_version": 1,
            "approval_ref": "SYNTHETIC-FIXTURE",
            "expires_at": "2099-01-01T00:00:00Z",
            "revoked": False,
            "pins": v.identities(pins, ROOT, config),
            "exceptions": [],
        }
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "authority.json"
            data = v.encode(record)
            path.write_bytes(data)
            self.assertEqual(v.authority(path, v.digest(data), pins, ROOT, config)["approval_ref"], "SYNTHETIC-FIXTURE")
            with self.assertRaisesRegex(v.Invalid, "authority-hash"):
                v.authority(path, "0" * 64, pins, ROOT, config)
            for change in [
                {"expires_at": "2000-01-01T00:00:00Z"},
                {"revoked": True},
                {"invented_approval": True},
                {"pins": {**record["pins"], "policy": "0" * 64}},
            ]:
                with self.subTest(change=change):
                    data = v.encode({**record, **change})
                    path.write_bytes(data)
                    with self.assertRaises(v.Invalid):
                        v.authority(path, v.digest(data), pins, ROOT, config)

    def test_timestamp_requires_utc_and_valid_date(self):
        for value in ["2026-09-29", "2026-09-29T00:00:00", "2026-02-30T00:00:00Z"]:
            with self.subTest(value=value), self.assertRaises(v.Invalid):
                v.stamp(value)


class ObservationTests(unittest.TestCase):
    def setUp(self):
        self.check = fixture()["checks"][0]

    def test_actual_contract_hits(self):
        observed, failures = v.go_observations(stream(events(self.check)), self.check)
        self.assertEqual(observed, {"STORAGE-ATOMIC": 7})
        self.assertEqual(failures, [])

    def test_non_vacuity_and_failure(self):
        for kind in ("empty", "no-marker", "budget", "skipped", "failed", "missing-terminal", "missing-test", "failed-after-marker"):
            with self.subTest(kind=kind):
                rows = events(self.check)
                if kind == "empty":
                    rows = []
                elif kind == "no-marker":
                    rows.pop(2)
                elif kind == "budget":
                    rows = events(self.check, 6)
                elif kind in ("failed", "failed-after-marker", "skipped"):
                    rows[3]["Action"] = "skip" if kind == "skipped" else "fail"
                elif kind == "missing-terminal":
                    rows.pop()
                elif kind == "missing-test":
                    rows = [rows[0], rows[-1]]
                _, failures = v.go_observations(stream(rows), self.check)
                self.assertTrue(failures)

    def test_malformed_native_stream(self):
        mutations = {
            "duplicate": lambda rows: rows.insert(3, rows[2]),
            "wrong-package": lambda rows: rows[0].update(Package="other"),
            "out-of-order": lambda rows: rows.pop(1),
            "late-output": lambda rows: rows.append(rows[2]),
            "invalid-hit": lambda rows: rows[2].update(Output='ASTRA-CONTRACT {"contract":"STORAGE-ATOMIC","hits":true}'),
            "zero-hit": lambda rows: rows[2].update(Output='ASTRA-CONTRACT {"contract":"STORAGE-ATOMIC","hits":0}'),
            "wrong-shape": lambda rows: rows[2].update(Output=[]),
        }
        for name, mutate in mutations.items():
            with self.subTest(name=name), self.assertRaises(v.Invalid):
                rows = events(self.check)
                mutate(rows)
                v.go_observations(stream(rows), self.check)

    def test_truncated_json(self):
        with self.assertRaises(v.Invalid):
            v.go_observations(stream(events(self.check))[:-4], self.check)


class DecisionTests(unittest.TestCase):
    def setUp(self):
        self.config = fixture()
        self.now = dt.datetime(2026, 9, 29, tzinfo=dt.timezone.utc)
        self.key = next(iter(v.expand(self.config)))
        self.observation = ("storage-examples", "linux-amd64-cgo", "STORAGE-ATOMIC")

    def result(self, observations=None, exceptions=None, hard=None):
        return v.decide(self.config, observations or {}, hard or [], exceptions or [], self.now)

    def exception(self):
        return {"id": "fixture", "obligation": self.key, "expires_at": "2099-01-01T00:00:00Z", "revoked": False}

    def test_passed_and_missing(self):
        self.assertEqual(self.result()["status"], "blocked")
        self.assertEqual(self.result({self.observation: "passed"})["status"], "eligible")

    def test_missing_platform(self):
        self.config["components"][0]["platforms"].append("second-platform")
        result = self.result({self.observation: "passed"})
        self.assertEqual(result["status"], "blocked")
        self.assertEqual(len(result["missing"]), 1)

    def test_pass_does_not_override_other_required_failure(self):
        self.config["components"][0]["requirements"][0]["checks"].append("other")
        result = self.result({self.observation: "passed", ("other", *self.observation[1:]): "failed"})
        self.assertEqual(result["missing"], [self.key])

    def test_exception_scope_expiry_and_revocation(self):
        self.assertEqual(self.result(exceptions=[self.exception()])["status"], "eligible-with-exceptions")
        for change in [{"expires_at": "2026-09-29T00:00:00Z"}, {"revoked": True}, {"obligation": "unknown"}]:
            with self.subTest(change=change):
                exception = {**self.exception(), **change}
                self.assertEqual(self.result(exceptions=[exception])["status"], "blocked")

    def test_hard_block_not_waived(self):
        self.assertEqual(self.result(exceptions=[self.exception()], hard=["source-changed"])["status"], "blocked")


class EvidenceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.config = fixture()
        self.check = self.config["checks"][0]
        self.pins = {"policy": "a" * 64, "rules": "b" * 64}
        self.approved = {"pins": {"collector": "c" * 64}, "artifact_sha256": "d" * 64, "exceptions": []}
        self.source = {"commit": "fixture-only", "files": [], "status": "fixture-only"}
        self.evidence = {
            "schema_version": 1,
            "run_id": "00000000-0000-4000-8000-000000000000",
            "kind": "native-local",
            "trust": "local",
            "platform": "linux-amd64-cgo",
            "system": "Linux",
            "machine": "x86_64",
            **self.pins,
            "collector": "c" * 64,
            "authority_sha256": "d" * 64,
            "authorization_ref": "SYNTHETIC-FIXTURE-NOT-PILOT",
            "started_at": "2026-09-29T00:00:00Z",
            "finished_at": "2026-09-29T00:00:01Z",
            "engine": {"python": "fixture", "jsonschema": "fixture"},
            "source_before": v.write_new(self.directory / "before.json", self.source),
            "source_after": v.write_new(self.directory / "after.json", self.source),
            "checks": [
                {
                    "id": "storage-examples",
                    "platform": "linux-amd64-cgo",
                    "argv": self.check["argv"],
                    "environment": self.check["environment"],
                    "resolved_environment": self.check["environment"],
                    "executable": "/fixture/go",
                    "executable_sha256": "e" * 64,
                    "toolchain": {"command": {"path": "/fixture/go", "sha256": "e" * 64}},
                    "started_at": "2026-09-29T00:00:00Z",
                    "finished_at": "2026-09-29T00:00:01Z",
                    "duration_ms": 1000,
                    "exit_code": 0,
                    "termination": "exited",
                    "complete": True,
                    "cleanup": True,
                    "stdout": v.write_new(self.directory / "stdout.log", stream(events(self.check))),
                    "stderr": v.write_new(self.directory / "stderr.log", b""),
                }
            ],
        }
        self.evidence["resources"] = {"cpus": 4, "available_bytes": 8 * 1024**3, "build_jobs": 4, "test_jobs": 4, "disk_free_bytes": 1024**3}

    def evaluate(self):
        self.evidence["records"] = []
        for index, record in enumerate(self.evidence["checks"]):
            path = self.directory / f"native-{index}.json"
            data = v.encode(record)
            path.write_bytes(data)
            self.evidence["records"].append({"path": path.name, "bytes": len(data), "sha256": v.digest(data)})
        (self.directory / "evidence.json").write_bytes(v.encode(self.evidence))
        with patch.object(v, "source", return_value=self.source):
            return v.gate(self.config, self.pins, self.approved, self.directory)

    def test_positive_fixture(self):
        self.assertEqual(self.evaluate()["status"], "eligible")

    def test_native_failures_remain_blocking(self):
        for changes in [
            {"exit_code": 1},
            {"complete": False},
            {"cleanup": False},
            {"termination": "interrupted"},
            {"termination": "timeout"},
            {"duration_ms": 999999},
        ]:
            with self.subTest(changes=changes):
                original = self.evidence["checks"][0].copy()
                self.evidence["checks"][0].update(changes)
                self.assertEqual(self.evaluate()["status"], "blocked")
                self.evidence["checks"][0] = original

    def test_integrity_rejected(self):
        (self.directory / "stdout.log").write_bytes(b"replaced")
        with self.assertRaisesRegex(v.Invalid, "artifact-integrity"):
            self.evaluate()

    def test_duplicate_attempt_and_self_report_rejected(self):
        self.evidence["checks"].append(copy.deepcopy(self.evidence["checks"][0]))
        with self.assertRaisesRegex(v.Invalid, "duplicate-or-unknown-check"):
            self.evaluate()
        self.evidence["checks"].pop()
        self.evidence["gate"] = {"status": "eligible"}
        with self.assertRaisesRegex(v.Invalid, "schema-invalid"):
            self.evaluate()

    def test_trust_and_convergence_gaps(self):
        self.config["components"][0]["evidence_trust"] = "ci"
        self.config["components"][0]["convergence"]["baseline"] = None
        result = self.evaluate()
        self.assertEqual(result["status"], "blocked")
        self.assertIn("trust-gap:polaris-storage", result["hard"])
        self.assertIn("convergence-unconfirmed:polaris-storage", result["hard"])

    def test_actual_source_mismatch(self):
        self.source["commit"] = "changed"
        self.assertIn("source-changed", self.evaluate()["hard"])

    def test_missing_record_not_green(self):
        self.evidence["checks"] = []
        self.assertEqual(self.evaluate()["status"], "blocked")


@unittest.skipUnless(platform.system() == "Linux", "Linux collector lifecycle")
class CollectorTests(unittest.TestCase):
    def test_process_exit_timeout_output_limit_and_orphan(self):
        # 每例在独立采集进程启用 subreaper, 不接管测试框架的其他子进程.
        script = """
import ctypes, json, pathlib, sys
from tools import verify as v
assert ctypes.CDLL(None).prctl(36, 1, 0, 0, 0) == 0
root=pathlib.Path(sys.argv[1]); directory=root/'build'; directory.mkdir()
check={'id':'native','argv':[sys.executable,'-c',sys.argv[2]],'environment':{},'timeout_s':1,'log_bytes':1024}
record=v.execute(check,root,directory,'fixture')
print(json.dumps({'record':record,'remaining':v.owned_members()}))
"""
        cases = [
            ("print('complete')", "exited", True),
            ("import time; time.sleep(30)", "timeout", False),
            ("print('x'*2048)", "log-limit", False),
            ("import os,time; child=os.fork(); os._exit(0) if child else None; os.setsid(); time.sleep(30)", "timeout", False),
        ]
        for code, termination, complete in cases:
            with self.subTest(termination=termination, code=code), tempfile.TemporaryDirectory() as directory:
                result = subprocess.run([sys.executable, "-B", "-c", script, directory, code], cwd=ROOT, capture_output=True, timeout=15, check=True)
                value = json.loads(result.stdout)
                self.assertEqual(value["record"]["termination"], termination)
                self.assertEqual(value["record"]["complete"], complete)
                self.assertTrue(value["record"]["cleanup"])
                self.assertEqual(value["remaining"], [])


if __name__ == "__main__":
    unittest.main()
