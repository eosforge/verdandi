#!/usr/bin/env python3
"""Run a fault-injected Registration/Selector endurance qualification.

The harness owns one authenticated Redis 8.8 container with AOF persistence,
one exact remote data directory, and one dedicated port. It samples Redis while
500 typed Go Registrations drive canonical Lua Updates and eight typed Selectors
run local One/Any load predictions, injects bounded cache/connection/process
faults, verifies final convergence and cleanup, then optionally runs the
independent Sentinel fault matrix.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import secrets
import shlex
import statistics
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any
from urllib.parse import quote

import redis

REPOSITORY = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPOSITORY))

from testkit.sentinel.sentinel_test import (  # noqa: E402
    QualificationError,
    Remote,
    port_open,
)


class Fixture:
    """Own one persistent, restartable Redis process and its exact data path."""

    def __init__(self, remote: Remote, run_id: str, port: int) -> None:
        self.remote = remote
        self.run_id = run_id
        self.port = port
        self.name = f"verdandi-soak-{run_id}"
        self.directory = f"/tmp/verdandi-soak-{run_id}"
        self.password = secrets.token_hex(24)
        self.created = False
        self.directory_created = False

    @property
    def url(self) -> str:
        return f"redis://default:{quote(self.password)}@" f"{self.remote.host}:{self.port}/0"

    def deploy(self) -> None:
        if port_open(self.remote.host, self.port):
            raise QualificationError(f"required test port is occupied: {self.port}")
        existing = set(self.remote.run("docker ps -a --format '{{.Names}}'").splitlines())
        if self.name in existing:
            raise QualificationError(f"container collision: {self.name}")
        self.remote.run(
            "set -eu; "
            f"test ! -e {shlex.quote(self.directory)}; "
            f"install -d -m 0777 {shlex.quote(self.directory)}; "
            f"printf %s {shlex.quote(self.run_id)} > "
            f"{shlex.quote(self.directory + '/owner')}"
        )
        self.directory_created = True
        command = [
            "docker",
            "run",
            "-d",
            "--name",
            self.name,
            "--label",
            f"verdandi.test={self.run_id}",
            "--label",
            "verdandi.kind=soak",
            "--mount",
            f"type=bind,src={self.directory},dst=/data",
            "-p",
            f"{self.port}:6379",
            "redis:8.8.0",
            "redis-server",
            "--appendonly",
            "yes",
            "--appendfsync",
            "everysec",
            "--save",
            "",
            "--requirepass",
            self.password,
        ]
        try:
            self.remote.run(" ".join(map(shlex.quote, command)))
            self.created = True
            self.wait_ready(30)
        except Exception:
            self.cleanup()
            raise

    def verify_owner(self) -> None:
        label = self.remote.run("docker inspect -f " + shlex.quote('{{index .Config.Labels "verdandi.test"}}') + " " + shlex.quote(self.name)).strip()
        if label != self.run_id:
            raise QualificationError(f"fixture ownership mismatch for {self.name}: {label!r}")

    def wait_ready(self, timeout: float) -> None:
        deadline = time.monotonic() + timeout
        last_error: Exception | None = None
        while time.monotonic() < deadline:
            try:
                client = redis.Redis.from_url(
                    self.url,
                    socket_connect_timeout=1,
                    socket_timeout=1,
                )
                try:
                    if client.ping():
                        return
                finally:
                    client.close()
            except Exception as error:  # readiness retains the last cause
                last_error = error
            time.sleep(0.1)
        raise QualificationError(f"Redis did not become ready: {last_error}")

    def restart(self) -> None:
        self.verify_owner()
        self.remote.run("docker restart -t 2 " + shlex.quote(self.name))
        self.wait_ready(30)

    def pause(self, seconds: float) -> None:
        self.verify_owner()
        command = (
            "set -eu; "
            f"docker pause {shlex.quote(self.name)} >/dev/null; "
            f"sleep {shlex.quote(str(seconds))}; "
            f"docker unpause {shlex.quote(self.name)} >/dev/null"
        )
        self.remote.run(command)
        self.wait_ready(15)

    def cleanup(self) -> None:
        if not self.created and not self.directory_created:
            return
        label = self.remote.run(
            "docker inspect -f " + shlex.quote('{{index .Config.Labels "verdandi.test"}}') + " " + shlex.quote(self.name),
            check=False,
        ).strip()
        if label and label != self.run_id:
            raise QualificationError(f"refusing to remove {self.name}: ownership label is {label!r}")
        if label == self.run_id:
            self.remote.run(f"docker rm -f {shlex.quote(self.name)}")
        directory = shlex.quote(self.directory)
        owner = shlex.quote(self.directory + "/owner")
        self.remote.run(
            "set -eu; "
            f"if test -e {directory}; then "
            f'test "$(cat {owner})" = {shlex.quote(self.run_id)}; '
            "docker run --rm "
            f"--mount type=bind,src={directory},dst=/cleanup "
            "redis:8.8.0 sh -c " + shlex.quote("find /cleanup -mindepth 1 -maxdepth 1 -exec rm -rf -- {} +") + "; "
            f"rmdir {directory}; fi"
        )
        self.created = False
        self.directory_created = False
        deadline = time.monotonic() + 10
        while port_open(self.remote.host, self.port) and time.monotonic() < deadline:
            time.sleep(0.1)
        if port_open(self.remote.host, self.port):
            raise QualificationError(f"test listener remains on port {self.port}")


@dataclass(frozen=True)
class Fault:
    at_seconds: float
    kind: str


class RedisMonitor:
    def __init__(
        self,
        redis_url: str,
        started: float,
        interval: float,
        sample_file: Path | None,
    ) -> None:
        self.redis_url = redis_url
        self.started = started
        self.interval = interval
        self.sample_file = sample_file
        self.samples: list[dict[str, Any]] = []
        self.failures: list[dict[str, Any]] = []
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, name="redis-monitor")
        if self.sample_file is not None:
            self.sample_file.parent.mkdir(parents=True, exist_ok=True)
            self.sample_file.write_text("", encoding="utf-8")

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        self._thread.join(timeout=max(10, self.interval * 2))
        if self._thread.is_alive():
            raise QualificationError("Redis monitor did not stop")

    def sample_now(self) -> dict[str, Any]:
        client = redis.Redis.from_url(
            self.redis_url,
            socket_connect_timeout=2,
            socket_timeout=3,
        )
        try:
            redis_seconds, redis_microseconds = client.time()
            server = client.info("server")
            memory = client.info("memory")
            stats = client.info("stats")
            clients = client.info("clients")
            cpu = client.info("cpu")
            commandstats = client.info("commandstats")
            evalsha = commandstats.get("cmdstat_evalsha", {})
            return {
                "elapsed_seconds": round(time.monotonic() - self.started, 3),
                "wall_time_unix_ms": time.time_ns() // 1_000_000,
                "redis_time_unix_ms": (redis_seconds * 1_000 + redis_microseconds // 1_000),
                "uptime_seconds": server.get("uptime_in_seconds", 0),
                "dbsize": client.dbsize(),
                "used_memory_bytes": memory.get("used_memory", 0),
                "used_memory_rss_bytes": memory.get("used_memory_rss", 0),
                "used_memory_peak_bytes": memory.get("used_memory_peak", 0),
                "mem_fragmentation_ratio": memory.get("mem_fragmentation_ratio", 0),
                "allocator_frag_ratio": memory.get("allocator_frag_ratio", 0),
                "connected_clients": clients.get("connected_clients", 0),
                "blocked_clients": clients.get("blocked_clients", 0),
                "total_commands_processed": stats.get("total_commands_processed", 0),
                "instantaneous_ops_per_sec": stats.get("instantaneous_ops_per_sec", 0),
                "total_net_input_bytes": stats.get("total_net_input_bytes", 0),
                "total_net_output_bytes": stats.get("total_net_output_bytes", 0),
                "expired_keys": stats.get("expired_keys", 0),
                "expired_subkeys": stats.get("expired_subkeys", 0),
                "evicted_keys": stats.get("evicted_keys", 0),
                "rejected_connections": stats.get("rejected_connections", 0),
                "evalsha_calls": evalsha.get("calls", 0),
                "evalsha_usecs": evalsha.get("usec", 0),
                "redis_cpu_user_seconds": cpu.get("used_cpu_user", 0),
                "redis_cpu_system_seconds": cpu.get("used_cpu_sys", 0),
            }
        finally:
            client.close()

    def _run(self) -> None:
        while not self._stop.is_set():
            try:
                sample = self.sample_now()
                self.samples.append(sample)
                self._append_sample("sample", sample)
                print(
                    "SOAK heartbeat "
                    f"elapsed={sample['elapsed_seconds']:.0f}s "
                    f"keys={sample['dbsize']} "
                    f"memory={sample['used_memory_bytes']} "
                    f"rss={sample['used_memory_rss_bytes']} "
                    f"ops={sample['instantaneous_ops_per_sec']} "
                    f"clients={sample['connected_clients']}",
                    flush=True,
                )
            except Exception as error:  # injected outages are recorded evidence
                failure = {
                    "elapsed_seconds": round(time.monotonic() - self.started, 3),
                    "error": str(error),
                }
                self.failures.append(failure)
                self._append_sample("failure", failure)
                print(
                    f"SOAK heartbeat unavailable elapsed={failure['elapsed_seconds']:.0f}s",
                    flush=True,
                )
            self._stop.wait(self.interval)

    def _append_sample(self, kind: str, value: dict[str, Any]) -> None:
        if self.sample_file is None:
            return
        record = {"kind": kind, **value}
        with self.sample_file.open("a", encoding="utf-8") as target:
            target.write(json.dumps(record, sort_keys=True) + "\n")


class FaultInjector:
    def __init__(
        self,
        fixture: Fixture,
        redis_url: str,
        started: float,
        faults: list[Fault],
    ) -> None:
        self.fixture = fixture
        self.redis_url = redis_url
        self.started = started
        self.faults = faults
        self.results: list[dict[str, Any]] = []
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, name="fault-injector")

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        self._thread.join(timeout=45)
        if self._thread.is_alive():
            raise QualificationError("fault injector did not stop")

    def _run(self) -> None:
        for fault in self.faults:
            remaining = self.started + fault.at_seconds - time.monotonic()
            if remaining > 0 and self._stop.wait(remaining):
                return
            if self._stop.is_set():
                return
            event: dict[str, Any] = {
                "kind": fault.kind,
                "scheduled_seconds": fault.at_seconds,
                "started_seconds": round(time.monotonic() - self.started, 3),
            }
            print(
                f"SOAK fault begin kind={fault.kind} " f"elapsed={event['started_seconds']:.0f}s",
                flush=True,
            )
            try:
                detail = self._inject(fault.kind)
                event["status"] = "pass"
                event["detail"] = detail
            except Exception as error:  # retained and failed by the main gate
                event["status"] = "fail"
                event["error"] = str(error)
            event["finished_seconds"] = round(
                time.monotonic() - self.started,
                3,
            )
            self.results.append(event)
            print(
                f"SOAK fault end kind={fault.kind} status={event['status']} " f"elapsed={event['finished_seconds']:.0f}s",
                flush=True,
            )
            if event["status"] != "pass":
                return

    def _inject(self, kind: str) -> Any:
        if kind == "pause":
            self.fixture.pause(3)
            return {"paused_seconds": 3}
        if kind == "restart":
            self.fixture.restart()
            return {"aof_persistence": True}
        client = redis.Redis.from_url(
            self.redis_url,
            socket_connect_timeout=2,
            socket_timeout=5,
        )
        try:
            if kind == "script_flush":
                return {"result": bool(client.script_flush())}
            if kind == "kill_pubsub":
                return {
                    "killed": client.client_kill_filter(
                        _type="pubsub",
                        skipme=True,
                    )
                }
            if kind == "kill_normal":
                return {
                    "killed": client.client_kill_filter(
                        _type="normal",
                        skipme=True,
                    )
                }
            raise QualificationError(f"unknown fault kind: {kind}")
        finally:
            client.close()


def build_faults(duration: int) -> list[Fault]:
    if duration < 600:
        points = (
            (0.15, "script_flush"),
            (0.28, "kill_pubsub"),
            (0.40, "pause"),
            (0.55, "kill_normal"),
            (0.70, "restart"),
            (0.85, "script_flush"),
        )
        return [Fault(max(5, duration * ratio), kind) for ratio, kind in points]

    reserved: list[Fault] = []
    for second in (1_200, 3_000, 4_800, 6_600):
        if second < duration - 120:
            reserved.append(Fault(second, "pause"))
    for second in (1_800, 3_600, 5_400):
        if second < duration - 120:
            reserved.append(Fault(second, "restart"))
    for second in (2_700, 6_300):
        if second < duration - 120:
            reserved.append(Fault(second, "kill_normal"))
    for second in range(750, duration - 120, 600):
        reserved.append(Fault(second, "kill_pubsub"))

    occupied = [fault.at_seconds for fault in reserved]
    for second in range(300, duration - 120, 300):
        if all(abs(second - current) >= 30 for current in occupied):
            reserved.append(Fault(second, "script_flush"))
    return sorted(reserved, key=lambda fault: fault.at_seconds)


def run_streamed(
    name: str,
    command: list[str],
    directory: Path,
    environment: dict[str, str],
) -> dict[str, Any]:
    print(f"\n=== {name} ===", flush=True)
    started = time.monotonic()
    process = subprocess.Popen(
        command,
        cwd=directory,
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        bufsize=1,
    )
    output: list[str] = []
    assert process.stdout is not None
    for line in process.stdout:
        # WSL may emit a localized UTF-16LE networking warning through a pipe
        # opened as UTF-8. It contains NULs and is unrelated to the child test.
        if "\x00" in line:
            continue
        console_encoding = sys.stdout.encoding or "utf-8"
        printable = line.encode(console_encoding, errors="replace").decode(console_encoding)
        print(printable, end="", flush=True)
        output.append(printable)
    status = process.wait()
    combined = "".join(output)
    if status != 0:
        raise QualificationError(f"{name} exited with status {status}; output tail:\n" + combined[-16_384:])
    return {
        "name": name,
        "status": "pass",
        "elapsed_seconds": round(time.monotonic() - started, 3),
        "output": combined.strip(),
    }


def run_go_soak(
    fixture: Fixture,
    duration: int,
    fanout: int,
    lifecycle_interval: str,
) -> tuple[dict[str, Any], dict[str, Any]]:
    environment = os.environ.copy()
    environment.update(
        {
            "VERDANDI_REDIS_URL": fixture.url,
            "VERDANDI_SOAK_SECONDS": str(duration),
            "VERDANDI_SELECTOR_FANOUT": str(fanout),
            "VERDANDI_SOAK_LIFECYCLE_INTERVAL": lifecycle_interval,
        }
    )
    command = [
        "go",
        "test",
        "-tags=integration,load,soak",
        "-run",
        "^TestRegistrationSelectorSoak$",
        "-count=1",
        # The workload is Redis-time gated. Leave local monotonic clocks one
        # hour of drift headroom without weakening the Redis duration floor.
        f"-timeout={duration + 3_600}s",
        "-v",
        "./registration",
    ]
    if os.name == "nt":
        forwarded = {
            name: environment[name]
            for name in (
                "VERDANDI_REDIS_URL",
                "VERDANDI_SOAK_SECONDS",
                "VERDANDI_SELECTOR_FANOUT",
                "VERDANDI_SOAK_LIFECYCLE_INTERVAL",
            )
        }
        script = "env " + " ".join(f"{name}={shlex.quote(value)}" for name, value in forwarded.items())
        script += " " + " ".join(map(shlex.quote, command))
        invocation = [
            "wsl.exe",
            "--cd",
            str(REPOSITORY / "sdk" / "go"),
            "--",
            "bash",
            "-lc",
            script,
        ]
        result = run_streamed(
            "Go Registration/Selector soak (WSL/Linux)",
            invocation,
            REPOSITORY,
            os.environ.copy(),
        )
    else:
        result = run_streamed(
            "Go Registration/Selector soak (Linux)",
            command,
            REPOSITORY / "sdk" / "go",
            environment,
        )
    matches = re.findall(r"SOAK_RESULT (\{.*\})", result["output"])
    if len(matches) != 1:
        raise QualificationError(f"Go soak emitted {len(matches)} structured results, want one")
    return result, json.loads(matches[0])


def run_post_checks(fixture: Fixture, phase: str) -> list[dict[str, Any]]:
    environment = os.environ.copy()
    environment["VERDANDI_REDIS_URL"] = fixture.url
    checks = [
        run_streamed(
            f"canonical Lua contract {phase}",
            [
                sys.executable,
                "-B",
                "testkit/lua/registration_test.py",
                "--redis-url",
                fixture.url,
            ],
            REPOSITORY,
            environment,
        ),
        run_streamed(
            f"Rust standalone convergence {phase}",
            [
                "cargo",
                "test",
                "--test",
                "integration",
                "registration_and_selector_reconcile_on_redis_8",
                "--",
                "--ignored",
                "--nocapture",
            ],
            REPOSITORY / "sdk" / "rust",
            environment,
        ),
        run_streamed(
            f"Rust typed Registration/Selector {phase}",
            [
                "cargo",
                "test",
                "--test",
                "integration",
                "typed_registration_and_transactional_selector",
                "--",
                "--ignored",
                "--exact",
                "--nocapture",
            ],
            REPOSITORY / "sdk" / "rust",
            environment,
        ),
    ]
    return checks


def median(values: list[int | float]) -> float:
    return float(statistics.median(values)) if values else 0.0


def redis_analysis(samples: list[dict[str, Any]]) -> dict[str, Any]:
    if len(samples) < 3:
        raise QualificationError(f"only {len(samples)} Redis samples were collected")
    stable = [sample for sample in samples if sample["dbsize"] >= 500]
    if len(stable) < 3:
        raise QualificationError("Redis sampling never observed the live population")
    width = max(3, len(stable) // 10)
    early = stable[:width]
    late = stable[-width:]
    early_memory = median([sample["used_memory_bytes"] for sample in early])
    late_memory = median([sample["used_memory_bytes"] for sample in late])
    memory_growth = late_memory - early_memory
    memory_gate = max(2 * 1024 * 1024, early_memory * 0.25)
    evicted = max(sample["evicted_keys"] for sample in samples)
    rejected = max(sample["rejected_connections"] for sample in samples)
    expired_keys = max(sample["expired_keys"] for sample in samples)
    expired_subkeys = max(sample["expired_subkeys"] for sample in samples)
    return {
        "samples": len(samples),
        "stable_samples": len(stable),
        "early_used_memory_median_bytes": early_memory,
        "late_used_memory_median_bytes": late_memory,
        "used_memory_growth_bytes": memory_growth,
        "used_memory_growth_gate_bytes": memory_gate,
        "used_memory_growth_pass": memory_growth <= memory_gate,
        "used_memory_peak_bytes": max(sample["used_memory_peak_bytes"] for sample in samples),
        "used_memory_rss_peak_bytes": max(sample["used_memory_rss_bytes"] for sample in samples),
        "fragmentation_ratio_peak": max(sample["mem_fragmentation_ratio"] for sample in samples),
        "connected_clients_peak": max(sample["connected_clients"] for sample in samples),
        "blocked_clients_peak": max(sample["blocked_clients"] for sample in samples),
        "instantaneous_ops_peak": max(sample["instantaneous_ops_per_sec"] for sample in samples),
        "expired_keys_peak_epoch_value": expired_keys,
        "expired_subkeys_peak_epoch_value": expired_subkeys,
        "evicted_keys_peak_epoch_value": evicted,
        "rejected_connections_peak_epoch_value": rejected,
        "eviction_pass": evicted == 0,
        "rejected_connection_pass": rejected == 0,
        "expiry_observed": expired_keys > 0 or expired_subkeys > 0,
    }


def run_sentinel(options: argparse.Namespace, password: str) -> dict[str, Any]:
    target = REPOSITORY / "testkit" / "results" / options.sentinel_result_name
    environment = os.environ.copy()
    environment[options.ssh_password_env] = password
    result = run_streamed(
        "post-soak Sentinel fault matrix",
        [
            sys.executable,
            "-B",
            "testkit/sentinel/sentinel_test.py",
            "--host",
            options.host,
            "--ssh-user",
            options.ssh_user,
            "--ssh-password-env",
            options.ssh_password_env,
            "--result-file",
            str(target),
        ],
        REPOSITORY,
        environment,
    )
    return {
        "suite": result,
        "result": json.loads(target.read_text(encoding="utf-8")),
        "result_file": (str(target.relative_to(REPOSITORY)) if target.is_relative_to(REPOSITORY) else str(target)),
    }


def source_fingerprint() -> dict[str, Any]:
    rust_sources = (
        "Cargo.lock",
        "Cargo.toml",
        "src/client.rs",
        "src/config.rs",
        "src/error.rs",
        "src/fields.rs",
        "src/identifier.rs",
        "src/lib.rs",
        "src/redis.rs",
        "tests/integration.rs",
        "tests/load.rs",
    )
    roots = (
        REPOSITORY / "lua" / "registration",
        REPOSITORY / "lua" / "src" / "registration",
        REPOSITORY / "sdk" / "go" / "registration",
        REPOSITORY / "sdk" / "rust" / "src" / "redis",
        REPOSITORY / "sdk" / "rust" / "src" / "registration",
        REPOSITORY / "testkit" / "sentinel",
    )
    go_root = REPOSITORY / "sdk" / "go"
    files = [go_root / "go.mod", go_root / "go.sum"]
    files.extend(
        path
        for path in go_root.glob("*.go")
        if path.is_file() and not path.name.endswith("_test.go")
    )
    files.extend(REPOSITORY / "sdk" / "rust" / name for name in rust_sources)
    files.extend(
        (
            REPOSITORY / "testkit" / "lua" / "generate_registration.py",
            REPOSITORY / "testkit" / "lua" / "registration_test.py",
            REPOSITORY / "testkit" / "soak" / "soak_test.py",
        )
    )
    for root in roots:
        for path in root.rglob("*"):
            if not path.is_file():
                continue
            if "target" in path.parts or "__pycache__" in path.parts:
                continue
            if path.suffix in {
                "",
                ".go",
                ".inc",
                ".json",
                ".lua",
                ".py",
                ".rs",
                ".toml",
                ".txt",
            } or path.name in {
                "go.mod",
                "go.sum",
                "Cargo.lock",
            }:
                files.append(path)
    digest = hashlib.sha256()
    relative_files: list[str] = []
    for path in sorted(set(files)):
        relative = path.relative_to(REPOSITORY).as_posix()
        content = path.read_bytes()
        digest.update(relative.encode("utf-8"))
        digest.update(b"\0")
        digest.update(content)
        digest.update(b"\0")
        relative_files.append(relative)
    return {
        "sha256": digest.hexdigest(),
        "files": len(relative_files),
        "paths": relative_files,
        "scope": "Registration/Selector and soak/Sentinel qualification sources",
    }


def write_result(options: argparse.Namespace, result: dict[str, Any]) -> None:
    if not options.result_file:
        return
    target = Path(options.result_file).resolve()
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="192.168.0.90")
    parser.add_argument("--ssh-user", default="ubuntu")
    parser.add_argument("--port", type=int, default=16380)
    parser.add_argument("--duration-seconds", type=int, default=7_200)
    parser.add_argument(
        "--minimum-redis-seconds",
        type=int,
        help="minimum elapsed Redis server time; defaults to --duration-seconds",
    )
    parser.add_argument("--selector-fanout", type=int, default=8)
    parser.add_argument("--sample-seconds", type=float, default=30)
    parser.add_argument("--lifecycle-interval", default="5m")
    parser.add_argument("--run-sentinel", action="store_true")
    parser.add_argument(
        "--sentinel-result-name",
        default="sentinel-soak-20260824.json",
    )
    parser.add_argument("--keep-container", action="store_true")
    parser.add_argument("--result-file")
    parser.add_argument(
        "--sample-file",
        help="optional JSONL path written and flushed after every Redis sample",
    )
    parser.add_argument(
        "--ssh-password-env",
        default="VERDANDI_TEST_SSH_PASSWORD",
    )
    options = parser.parse_args()
    if options.sample_file is None and options.result_file:
        result_path = Path(options.result_file)
        options.sample_file = str(result_path.with_name(result_path.stem + "-samples.jsonl"))
    if options.minimum_redis_seconds is None:
        options.minimum_redis_seconds = options.duration_seconds
    if not 1 <= options.port <= 65_535:
        parser.error("--port must be 1..65535")
    if not 30 <= options.duration_seconds <= 86_400:
        parser.error("--duration-seconds must be 30..86400")
    if not 30 <= options.minimum_redis_seconds <= options.duration_seconds:
        parser.error("--minimum-redis-seconds must be 30..--duration-seconds")
    if not 2 <= options.selector_fanout <= 64:
        parser.error("--selector-fanout must be 2..64")
    if not 5 <= options.sample_seconds <= 300:
        parser.error("--sample-seconds must be 5..300")
    return options


def main() -> int:
    options = arguments()
    password = os.environ.get(options.ssh_password_env)
    if not password:
        print(f"missing {options.ssh_password_env}", file=sys.stderr)
        return 2
    run_id = secrets.token_hex(4)
    remote = Remote(options.host, options.ssh_user, password)
    fixture = Fixture(remote, run_id, options.port)
    monitor: RedisMonitor | None = None
    injector: FaultInjector | None = None
    started = time.monotonic()
    result: dict[str, Any] | None = None
    try:
        fixture.deploy()
        source_before = source_fingerprint()
        pre_checks = run_post_checks(fixture, "before soak")
        preflight_client = redis.Redis.from_url(fixture.url)
        try:
            preflight_dbsize = preflight_client.dbsize()
        finally:
            preflight_client.close()
        if preflight_dbsize != 0:
            raise QualificationError(f"pre-soak checks left {preflight_dbsize} Redis keys")
        sample_file = Path(options.sample_file).resolve() if options.sample_file else None
        monitor = RedisMonitor(
            fixture.url,
            started,
            options.sample_seconds,
            sample_file,
        )
        injector = FaultInjector(
            fixture,
            fixture.url,
            started,
            build_faults(options.minimum_redis_seconds),
        )
        monitor.start()
        injector.start()
        go_suite, go_result = run_go_soak(
            fixture,
            options.duration_seconds,
            options.selector_fanout,
            options.lifecycle_interval,
        )
        injector.stop()
        final_soak_sample = monitor.sample_now()
        monitor.samples.append(final_soak_sample)
        monitor._append_sample("sample", final_soak_sample)
        monitor.stop()
        redis_elapsed_seconds = (final_soak_sample["redis_time_unix_ms"] - monitor.samples[0]["redis_time_unix_ms"]) / 1_000
        if redis_elapsed_seconds < options.minimum_redis_seconds:
            raise QualificationError(
                "Redis server elapsed time did not reach the qualification floor: " f"{redis_elapsed_seconds:.3f}s < " f"{options.minimum_redis_seconds}s"
            )
        source_after = source_fingerprint()
        if source_after["sha256"] != source_before["sha256"]:
            raise QualificationError("Go/Lua source changed during the endurance interval: " f"{source_before['sha256']} -> {source_after['sha256']}")

        fault_results = list(injector.results)
        if len(fault_results) != len(injector.faults):
            raise QualificationError(f"completed faults={len(fault_results)}, " f"scheduled={len(injector.faults)}")
        failed_faults = [fault for fault in fault_results if fault.get("status") != "pass"]
        if failed_faults:
            raise QualificationError(f"fault injection failures: {failed_faults}")

        analysis = redis_analysis(monitor.samples)
        if not analysis["used_memory_growth_pass"]:
            raise QualificationError(
                "Redis used_memory grew beyond the endurance gate: " f"{analysis['used_memory_growth_bytes']} > " f"{analysis['used_memory_growth_gate_bytes']}"
            )
        if not analysis["eviction_pass"]:
            raise QualificationError("Redis evicted keys during the soak")
        if not analysis["rejected_connection_pass"]:
            raise QualificationError("Redis rejected connections during the soak")
        if not analysis["expiry_observed"]:
            raise QualificationError("Redis expiry counters did not advance")
        if go_result["final_generation"] <= 1:
            raise QualificationError("Selectors did not publish a post-fault generation")

        post_checks = run_post_checks(fixture, "after soak")
        final_client = redis.Redis.from_url(fixture.url)
        try:
            final_dbsize = final_client.dbsize()
            final_server = final_client.info("server")
            final_memory = final_client.info("memory")
            final_stats = final_client.info("stats")
        finally:
            final_client.close()
        if final_dbsize != 0:
            raise QualificationError(f"successful soak left {final_dbsize} Redis keys")

        result = {
            "status": "standalone_pass",
            "run_id": run_id,
            "duration_seconds": options.duration_seconds,
            "minimum_redis_seconds": options.minimum_redis_seconds,
            "redis_elapsed_seconds": redis_elapsed_seconds,
            "elapsed_seconds": round(time.monotonic() - started, 3),
            "redis_version": final_server["redis_version"],
            "redis_persistence": "AOF everysec on an owned bind directory",
            "source_fingerprint": source_after,
            "selector_fanout": options.selector_fanout,
            "lifecycle_interval": options.lifecycle_interval,
            "sample_seconds": options.sample_seconds,
            "go_suite": go_suite,
            "go_result": go_result,
            "faults": fault_results,
            "redis_samples": monitor.samples,
            "redis_sample_failures": monitor.failures,
            "redis_sample_file": (
                str(sample_file.relative_to(REPOSITORY))
                if sample_file is not None and sample_file.is_relative_to(REPOSITORY)
                else str(sample_file) if sample_file is not None else None
            ),
            "redis_analysis": analysis,
            "pre_checks": pre_checks,
            "post_checks": post_checks,
            "sentinel": None,
            "final": {
                "dbsize": final_dbsize,
                "used_memory_bytes": final_memory["used_memory"],
                "used_memory_rss_bytes": final_memory["used_memory_rss"],
                "used_memory_peak_bytes": final_memory["used_memory_peak"],
                "total_commands_processed": final_stats["total_commands_processed"],
                "evicted_keys": final_stats["evicted_keys"],
                "rejected_connections": final_stats["rejected_connections"],
            },
        }
        write_result(options, result)
        if options.run_sentinel:
            result["sentinel"] = run_sentinel(options, password)
        result["status"] = "pass"
        result["elapsed_seconds"] = round(time.monotonic() - started, 3)
        write_result(options, result)
        serialized = json.dumps(result, indent=2, sort_keys=True)
        print(serialized)
        return 0
    except Exception as error:
        if result is None:
            result = {
                "status": "failed",
                "run_id": run_id,
                "duration_seconds": options.duration_seconds,
                "minimum_redis_seconds": options.minimum_redis_seconds,
                "elapsed_seconds": round(time.monotonic() - started, 3),
                "failure": str(error),
                "source_fingerprint": (source_before if "source_before" in locals() else None),
                "faults": list(injector.results) if injector is not None else [],
                "redis_samples": list(monitor.samples) if monitor is not None else [],
                "redis_sample_failures": (list(monitor.failures) if monitor is not None else []),
            }
        else:
            result["status"] = "standalone_pass_postcheck_failed"
            result["failure"] = str(error)
            result["elapsed_seconds"] = round(time.monotonic() - started, 3)
        write_result(options, result)
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    finally:
        if injector is not None:
            injector.stop()
        if monitor is not None:
            monitor.stop()
        if not options.keep_container:
            fixture.cleanup()
        remote.close()


if __name__ == "__main__":
    raise SystemExit(main())
