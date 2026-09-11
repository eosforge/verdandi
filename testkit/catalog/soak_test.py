#!/usr/bin/env python3
"""Run an interruptible, fault-injected Catalog endurance qualification."""

from __future__ import annotations

import argparse
from contextlib import ExitStack
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import shlex
import statistics
import subprocess
import sys
import time
from typing import Any

import redis

REPOSITORY = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPOSITORY))

from testkit.sentinel.sentinel_test import QualificationError, Remote  # noqa: E402
from testkit.soak.soak_test import (  # noqa: E402
    Fault,
    FaultInjector,
    Fixture,
    RedisMonitor,
    run_streamed,
)

HEARTBEAT = re.compile(r"CATALOG_SOAK_HEARTBEAT (\{.*\})")
FINAL_RESULT = re.compile(r"CATALOG_SOAK_RESULT (\{.*\})")


class CatalogSoakInterrupted(Exception):
    """The managed Go test received an intentional console interrupt."""


from testkit.support import run_command


class CatalogDriver:
    def __init__(self, command, directory, environment, on_output=None):
        self.command, self.directory, self.environment = command, directory, environment
        self.heartbeats, self.output_tail = [], []
        self.on_output = on_output

    def observe(self, text):
        if self.on_output is not None:
            self.on_output(text)
        for line in text.splitlines():
            self.output_tail.append(line)
            if match := HEARTBEAT.search(line):
                self.heartbeats.append(json.loads(match.group(1)))
        del self.output_tail[:-200]
        del self.heartbeats[:-256]

    def run(self):
        result = run_command(
            "Go Catalog Publisher/Subscriber soak",
            self.command,
            self.directory,
            self.environment,
            timeout=int(self.environment["VERDANDI_SOAK_SECONDS"]) + 1800,
            on_output=self.observe,
        )
        finals = [json.loads(match.group(1)) for line in self.output_tail if (match := FINAL_RESULT.search(line))]
        if len(finals) != 1:
            raise QualificationError(f"Catalog soak emitted {len(finals)} results, want one")
        return result, finals[0]

    def stop(self):
        # run_command owns the complete process lifetime even when it raises.
        pass


def build_catalog_faults(duration: int) -> list[Fault]:
    if duration < 7200:
        return [
            Fault(max(5, duration * ratio), kind)
            for ratio, kind in (
                (0.15, "script_flush"),
                (0.27, "kill_pubsub"),
                (0.39, "kill_normal"),
                (0.50, "pause"),
                (0.69, "restart"),
                (0.86, "script_flush"),
            )
        ]

    reserved: list[Fault] = []
    schedules = (
        (900, 900, "script_flush"),
        (1_200, 1_800, "kill_pubsub"),
        (1_500, 1_800, "kill_normal"),
        (2_100, 3_600, "pause"),
        (3_900, 7_200, "restart"),
    )
    occupied: list[float] = []
    for first, interval, kind in schedules:
        for second in range(first, duration - 120, interval):
            if all(abs(second - current) >= 30 for current in occupied):
                reserved.append(Fault(second, kind))
                occupied.append(second)
    return sorted(reserved, key=lambda fault: fault.at_seconds)


def catalog_timeout_seconds(duration: int) -> int:
    """Bound a Redis-clock soak without assuming the WSL clock runs at the same rate."""
    clock_skew_margin = max(600, duration // 8)
    shutdown_margin = 600
    return duration + clock_skew_margin + shutdown_margin


def catalog_command(fixture: Fixture, options: argparse.Namespace) -> tuple[list[str], Path, dict[str, str]]:
    environment = os.environ.copy()
    forwarded = {
        "VERDANDI_REDIS_URL": fixture.url,
        "VERDANDI_SOAK_SECONDS": str(options.duration_seconds),
        "VERDANDI_CATALOG_SOAK_CATALOGS": str(options.catalogs),
        "VERDANDI_CATALOG_SOAK_FIELDS": str(options.fields),
        "VERDANDI_CATALOG_SUBSCRIBER_FANOUT": str(options.subscriber_fanout),
        "VERDANDI_CATALOG_PERSISTENT_SUBSCRIBERS": str(options.persistent_subscribers),
        "VERDANDI_CATALOG_SOAK_RATE": str(options.rate),
    }
    command = [
        "go",
        "test",
        "-tags=integration,load,soak",
        "-run",
        "^TestCatalogSoak$",
        "-count=1",
        f"-timeout={catalog_timeout_seconds(options.duration_seconds)}s",
        "-v",
        "./catalog",
    ]
    environment.update(forwarded)
    return command, REPOSITORY / "sdk/go", environment


def source_fingerprint() -> dict[str, Any]:
    roots = (
        REPOSITORY / "lua" / "catalog",
        REPOSITORY / "lua" / "src" / "catalog",
        REPOSITORY / "sdk" / "go" / "catalog",
        REPOSITORY / "sdk" / "rust" / "src" / "catalog",
        REPOSITORY / "testkit" / "catalog",
    )
    files = [
        Path(__file__).resolve(),
        REPOSITORY / "sdk" / "rust" / "tests" / "catalog_v2.rs",
        REPOSITORY / "testkit" / "lua" / "catalog_test.py",
    ]
    for root in roots:
        for path in root.rglob("*"):
            if not path.is_file() or "target" in path.parts or "__pycache__" in path.parts:
                continue
            if path.suffix in {
                ".go",
                ".rs",
                ".lua",
                ".inc",
                ".json",
                ".py",
                ".toml",
                ".mod",
                ".sum",
                ".lock",
            }:
                files.append(path)
    digest = hashlib.sha256()
    relative: list[str] = []
    for path in sorted(set(files)):
        name = path.relative_to(REPOSITORY).as_posix()
        digest.update(name.encode())
        digest.update(b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
        relative.append(name)
    return {
        "sha256": digest.hexdigest(),
        "files": len(relative),
        "scope": "Catalog Lua, Go/Rust Catalog SDK, and Catalog soak harness",
    }


def median(values: list[int | float]) -> float:
    return float(statistics.median(values)) if values else 0.0


def reset_aware_delta(samples: list[dict[str, Any]], field: str) -> int:
    return sum(max(0, int(current[field]) - int(previous[field])) for previous, current in zip(samples, list(samples)[1:], strict=False))


def analyze_samples(samples: list[dict[str, Any]]) -> dict[str, Any]:
    if len(samples) < 2:
        return {"samples": len(samples), "status": "insufficient"}
    stable = [sample for sample in samples if sample["dbsize"] > 1]
    selected = stable if len(stable) >= 2 else samples
    width = max(1, len(selected) // 10)
    early_memory = median([sample["used_memory_bytes"] for sample in selected[:width]])
    late_memory = median([sample["used_memory_bytes"] for sample in selected[-width:]])
    growth = late_memory - early_memory
    gate = max(4 * 1024 * 1024, early_memory * 0.25)
    return {
        "samples": len(samples),
        "stable_samples": len(stable),
        "early_used_memory_median_bytes": early_memory,
        "late_used_memory_median_bytes": late_memory,
        "used_memory_growth_bytes": growth,
        "used_memory_growth_gate_bytes": gate,
        "used_memory_growth_pass": growth <= gate,
        "used_memory_peak_bytes": max(sample["used_memory_peak_bytes"] for sample in samples),
        "used_memory_rss_peak_bytes": max(sample["used_memory_rss_bytes"] for sample in samples),
        "connected_clients_peak": max(sample["connected_clients"] for sample in samples),
        "blocked_clients_peak": max(sample["blocked_clients"] for sample in samples),
        "instantaneous_ops_peak": max(sample["instantaneous_ops_per_sec"] for sample in samples),
        "commands_during_samples": reset_aware_delta(samples, "total_commands_processed"),
        "evalsha_during_samples": reset_aware_delta(samples, "evalsha_calls"),
        "evicted_keys": max(sample["evicted_keys"] for sample in samples),
        "rejected_connections": max(sample["rejected_connections"] for sample in samples),
    }


def post_checks(fixture: Fixture) -> list[dict[str, Any]]:
    environment = os.environ.copy()
    environment["VERDANDI_REDIS_URL"] = fixture.url
    return [
        run_streamed(
            "Catalog Lua contract after soak",
            [
                sys.executable,
                "-B",
                "testkit/lua/catalog_test.py",
                "--redis-url",
                fixture.url,
            ],
            REPOSITORY,
            environment,
        ),
        run_streamed(
            "Rust Catalog v1 convergence/checkpoint after soak",
            [
                "cargo",
                "test",
                "--test",
                "catalog_v2",
                "--",
                "--nocapture",
            ],
            REPOSITORY / "sdk" / "rust",
            environment,
        ),
        run_streamed(
            "Catalog-only Go/Rust interoperability after soak",
            [
                sys.executable,
                "-B",
                "testkit/catalog/interop_test.py",
                "--redis-url",
                fixture.url,
            ],
            REPOSITORY,
            environment,
        ),
    ]


def write_result(path: str | None, value: dict[str, Any]) -> None:
    if not path:
        return
    target = Path(path).resolve()
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def options() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="192.168.0.119")
    parser.add_argument("--ssh-user", default="ubuntu")
    parser.add_argument("--port", type=int, default=36440)
    parser.add_argument("--duration-seconds", type=int, default=86_400)
    parser.add_argument(
        "--minimum-redis-seconds",
        type=int,
        help="minimum elapsed Redis server time; defaults to --duration-seconds",
    )
    parser.add_argument("--sample-seconds", type=float, default=30)
    parser.add_argument("--catalogs", type=int, default=16)
    parser.add_argument("--fields", type=int, default=256)
    parser.add_argument("--subscriber-fanout", type=int, default=2)
    parser.add_argument("--persistent-subscribers", type=int, default=1)
    parser.add_argument("--rate", type=int, default=128)
    parser.add_argument("--no-faults", action="store_true")
    parser.add_argument("--keep-container", action="store_true")
    parser.add_argument("--result-file")
    parser.add_argument("--sample-file")
    parser.add_argument("--ssh-password-env", default="VERDANDI_TEST_SSH_PASSWORD")
    arguments = parser.parse_args()
    if arguments.sample_file is None and arguments.result_file:
        result = Path(arguments.result_file)
        arguments.sample_file = str(result.with_name(result.stem + "-samples.jsonl"))
    if arguments.minimum_redis_seconds is None:
        arguments.minimum_redis_seconds = arguments.duration_seconds
    if not 1 <= arguments.port <= 65_535:
        parser.error("--port must be 1..65535")
    if not 30 <= arguments.duration_seconds <= 86_400:
        parser.error("--duration-seconds must be 30..86400")
    if not 30 <= arguments.minimum_redis_seconds <= arguments.duration_seconds:
        parser.error("--minimum-redis-seconds must be 30..--duration-seconds")
    if not 5 <= arguments.sample_seconds <= 300:
        parser.error("--sample-seconds must be 5..300")
    if not 1 <= arguments.catalogs <= 64:
        parser.error("--catalogs must be 1..64")
    if not 1 <= arguments.fields <= 1024:
        parser.error("--fields must be 1..1024")
    if not 1 <= arguments.subscriber_fanout <= 8:
        parser.error("--subscriber-fanout must be 1..8")
    if not 0 <= arguments.persistent_subscribers <= arguments.subscriber_fanout:
        parser.error("--persistent-subscribers must be 0..--subscriber-fanout")
    if not 1 <= arguments.rate <= 2000:
        parser.error("--rate must be 1..2000")
    return arguments


def main() -> int:
    arguments = options()
    ssh_password = os.environ.get(arguments.ssh_password_env)
    if not ssh_password:
        print(f"missing {arguments.ssh_password_env}", file=sys.stderr)
        return 2
    run_id = secrets.token_hex(4)
    remote = Remote(arguments.host, arguments.ssh_user, ssh_password)
    fixture = Fixture(remote, run_id, arguments.port)
    started = time.monotonic()
    monitor: RedisMonitor | None = None
    injector: FaultInjector | None = None
    driver: CatalogDriver | None = None
    result: dict[str, Any] = {
        "status": "starting",
        "run_id": run_id,
        "duration_seconds": arguments.duration_seconds,
        "minimum_redis_seconds": arguments.minimum_redis_seconds,
        "port": arguments.port,
        "catalogs": arguments.catalogs,
        "fields": arguments.fields,
        "subscriber_fanout": arguments.subscriber_fanout,
        "persistent_subscribers": arguments.persistent_subscribers,
        "rate": arguments.rate,
    }
    interrupted = False
    completed = False
    failure: BaseException | None = None
    try:
        fixture.deploy()
        source_before = source_fingerprint()
        sample_file = Path(arguments.sample_file).resolve() if arguments.sample_file else None
        monitor = RedisMonitor(fixture.url, started, arguments.sample_seconds, sample_file)
        faults = [] if arguments.no_faults else build_catalog_faults(arguments.minimum_redis_seconds)
        injector = FaultInjector(fixture, fixture.url, started, faults)
        command, directory, environment = catalog_command(fixture, arguments)
        driver = CatalogDriver(command, directory, environment, injector.observe)
        monitor.start()
        suite, go_result = driver.run()
        injector.stop()
        final_sample = monitor.sample_now()
        monitor.samples.append(final_sample)
        monitor._append_sample("sample", final_sample)
        monitor.stop()
        redis_elapsed_seconds = (final_sample["redis_time_unix_ms"] - monitor.samples[0]["redis_time_unix_ms"]) / 1_000
        if redis_elapsed_seconds < arguments.minimum_redis_seconds:
            raise QualificationError(
                "Redis server elapsed time did not reach the Catalog qualification floor: "
                f"{redis_elapsed_seconds:.3f}s < "
                f"{arguments.minimum_redis_seconds}s"
            )
        source_after = source_fingerprint()
        if source_after["sha256"] != source_before["sha256"]:
            raise QualificationError(f"Catalog source changed during soak: {source_before['sha256']} -> {source_after['sha256']}")
        if len(injector.results) != len(faults):
            raise QualificationError(f"completed faults={len(injector.results)}, scheduled={len(faults)}")
        if any(item.get("status") != "pass" for item in injector.results):
            raise QualificationError(f"fault injection failures: {injector.results}")
        analysis = analyze_samples(monitor.samples)
        if not analysis.get("used_memory_growth_pass", False):
            raise QualificationError("Catalog Redis memory growth exceeded the soak gate")
        if analysis["evicted_keys"] != 0 or analysis["rejected_connections"] != 0:
            raise QualificationError("Redis eviction or rejected connection observed")
        checks = post_checks(fixture)
        final_client = redis.Redis.from_url(fixture.url)
        try:
            final_dbsize = final_client.dbsize()
            server = final_client.info("server")
        finally:
            final_client.close()
        if final_dbsize != 0:
            raise QualificationError(f"successful Catalog soak left {final_dbsize} keys")
        result.update(
            {
                "status": "pass",
                "elapsed_seconds": round(time.monotonic() - started, 3),
                "redis_elapsed_seconds": redis_elapsed_seconds,
                "redis_version": server["redis_version"],
                "redis_persistence": "AOF everysec on an owned bind directory",
                "source_fingerprint": source_after,
                "go_suite": suite,
                "go_result": go_result,
                "heartbeats": driver.heartbeats,
                "faults": injector.results,
                "redis_samples": list(monitor.samples),
                "redis_sample_failures": monitor.failures,
                "redis_analysis": analysis,
                "post_checks": checks,
                "final_dbsize": final_dbsize,
            }
        )
        write_result(arguments.result_file, result)
        print(json.dumps(result, indent=2, sort_keys=True))
        completed = True
        return 0
    except (KeyboardInterrupt, CatalogSoakInterrupted) as error:
        interrupted = True
        failure = error
    except BaseException as error:  # result evidence is retained before cleanup
        failure = error
    finally:
        with ExitStack() as cleanup:
            cleanup.callback(remote.close)
            if not arguments.keep_container:
                cleanup.callback(fixture.cleanup)
            if monitor is not None:
                cleanup.callback(monitor.stop)
            if injector is not None:
                cleanup.callback(injector.stop)
            if driver is not None:
                cleanup.callback(driver.stop)
            if not completed:
                result.update(
                    {
                        "status": "interrupted" if interrupted else "failed",
                        "elapsed_seconds": round(time.monotonic() - started, 3),
                        "heartbeats": driver.heartbeats if driver is not None else [],
                        "latest_heartbeat": driver.heartbeats[-1] if driver is not None and driver.heartbeats else None,
                        "driver_output_tail": driver.output_tail if driver is not None else [],
                        "faults": injector.results if injector is not None else [],
                        "redis_samples": list(monitor.samples) if monitor is not None else [],
                        "redis_sample_failures": monitor.failures if monitor is not None else [],
                        "redis_analysis": analyze_samples(monitor.samples) if monitor is not None else {},
                        "failure": type(failure).__name__ if interrupted else str(failure),
                    }
                )
                write_result(arguments.result_file, result)
    if interrupted:
        print(json.dumps(result, indent=2, sort_keys=True))
        return 130
    print(f"FAIL: {failure}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
