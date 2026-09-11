#!/usr/bin/env python3
"""Qualify Verdandi against one isolated remote Redis 8 container.

The harness owns one randomly named and labelled container on a preflighted
port. It runs the canonical Lua, Go, Rust, interoperability, sustained-load,
and population-recovery suites, then verifies that successful tests left the
isolated database empty before removing only that exact container.
"""

from __future__ import annotations

import argparse
import json
import os
import secrets
import shlex
import subprocess
import sys
import time
from pathlib import Path
from urllib.parse import quote

import redis

REPOSITORY = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPOSITORY))

from testkit.resources import Resources
from testkit.support import run_command

from testkit.sentinel.sentinel_test import (  # noqa: E402
    QualificationError,
    Remote,
    port_open,
)


class Fixture:
    def __init__(self, remote: Remote, run_id: str, port: int) -> None:
        self.remote = remote
        self.run_id = run_id
        self.port = port
        self.name = f"verdandi-it-{run_id}-standalone"
        self.password = secrets.token_hex(24)
        self.created = False
        self.resources = Resources(remote, run_id)

    @property
    def url(self) -> str:
        return f"redis://default:{quote(self.password)}@" f"{self.remote.host}:{self.port}/0"

    def deploy(self) -> None:
        if port_open(self.remote.host, self.port):
            raise QualificationError(f"required test port is occupied: {self.port}")
        existing = set(self.remote.run("docker ps -a --format '{{.Names}}'").splitlines())
        if self.name in existing:
            raise QualificationError(f"container collision: {self.name}")
        self.resources.container(self.name)
        command = [
            "docker",
            "run",
            "--pull",
            "never",
            "--memory",
            "512m",
            "--memory-swap",
            "512m",
            "--cpus",
            "1",
            "--pids-limit",
            "128",
            "-d",
            "--name",
            self.name,
            "--label",
            f"verdandi.test={self.run_id}",
            "--label",
            "verdandi.kind=standalone",
            "--tmpfs",
            "/data",
            "-p",
            f"{self.port}:6379",
            "redis:8.8.0",
            "redis-server",
            "--save",
            "",
            "--appendonly",
            "no",
            "--requirepass",
            self.password,
        ]
        self.remote.run(" ".join(map(shlex.quote, command)))
        self.created = True
        deadline = time.monotonic() + 20
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
            except Exception as error:  # readiness polling retains the cause
                last_error = error
            time.sleep(0.1)
        raise QualificationError(f"Redis did not become ready: {last_error}")

    def cleanup(self) -> None:
        self.resources.cleanup()


def run_go_load(environment):
    return run_command(
        "Go sustained update and renewal qualification",
        ["go", "test", "-tags=integration,load", "-run", "Qualification|Load|Scale", "-count=1", "-timeout=90m", "-v", "./registration"],
        REPOSITORY / "sdk/go",
        environment,
        timeout=5400,
    )


def run_go_race_integration(environment):
    if os.name == "nt":
        return {"name": "Go race detector", "status": "not_applicable", "reason": "Runs on the Linux target"}
    return run_command("Go integration race", ["go", "test", "-race", "-tags=integration", "-count=1", "./..."], REPOSITORY / "sdk/go", environment)


def qualify(fixture: Fixture, options: argparse.Namespace) -> dict[str, object]:
    environment = os.environ.copy()
    environment.update(
        {
            "VERDANDI_REDIS_URL": fixture.url,
            "VERDANDI_LOAD_SECONDS": str(options.load_seconds),
            "VERDANDI_SELECTOR_FANOUT": str(options.selector_fanout),
            "VERDANDI_SCALE_REGISTRATIONS": str(options.scale_registrations),
        }
    )
    results: list[dict[str, object]] = []
    results.append(
        run_command(
            "Registration Lua contract",
            [
                sys.executable,
                "-B",
                "testkit/lua/registration_test.py",
                "--redis-url",
                fixture.url,
            ],
            REPOSITORY,
            environment,
        )
    )
    results.append(
        run_command(
            "Catalog Lua contract",
            [
                sys.executable,
                "-B",
                "testkit/lua/catalog_test.py",
                "--redis-url",
                fixture.url,
            ],
            REPOSITORY,
            environment,
        )
    )
    results.append(
        run_command(
            "Go standalone integration",
            ["go", "test", "-tags=integration", "-count=1", "./..."],
            REPOSITORY / "sdk" / "go",
            environment,
        )
    )
    results.append(run_go_race_integration(environment))
    rust_tests = (
        "registration_and_selector_reconcile_on_redis_8",
        "registration_update_resets_automatic_renew",
        "typed_registration_and_transactional_selector",
        "zone_configuration_refreshes_without_restart",
        "dropping_last_registration_client_handle_stops_owned_workers",
        "protocol_ceiling_registration_recovery",
    )
    for test in rust_tests:
        results.append(
            run_command(
                f"Rust standalone integration: {test}",
                [
                    "cargo",
                    "test",
                    "--test",
                    "integration",
                    test,
                    "--",
                    "--ignored",
                    "--nocapture",
                ],
                REPOSITORY / "sdk" / "rust",
                environment,
                required_output="test result: ok. 1 passed;",
            )
        )
    results.append(
        run_command(
            "Rust Catalog v1 integration",
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
            required_output="test result: ok. 2 passed;",
        )
    )
    for test in (
        "root_commands_redis_integration",
        "one_transport_supports_independent_registration_zones",
    ):
        results.append(
            run_command(
                f"Rust root Redis API integration: {test}",
                [
                    "cargo",
                    "test",
                    "--test",
                    "root_redis",
                    test,
                    "--",
                    "--ignored",
                    "--nocapture",
                ],
                REPOSITORY / "sdk" / "rust",
                environment,
                required_output="test result: ok. 1 passed;",
            )
        )
    results.append(
        run_command(
            "Go/Rust live interoperability",
            [
                sys.executable,
                "-B",
                "testkit/interop/interop_test.py",
                "--redis-url",
                fixture.url,
            ],
            REPOSITORY,
            environment,
        )
    )
    if not options.skip_load:
        results.append(run_go_load(environment))
        for test in (
            "registration_selector_qualification_load",
            "registration_selector_renewal_load",
            "registration_selector_scale_recovery",
        ):
            results.append(
                run_command(
                    f"Rust load: {test}",
                    [
                        "cargo",
                        "test",
                        "--release",
                        "--test",
                        "load",
                        test,
                        "--",
                        "--ignored",
                        "--nocapture",
                    ],
                    REPOSITORY / "sdk" / "rust",
                    environment,
                    required_output="test result: ok. 1 passed;",
                )
            )

    client = redis.Redis.from_url(fixture.url)
    try:
        keys = sorted(key.decode(errors="replace") for key in client.scan_iter())
        if keys:
            raise QualificationError(f"successful suites left Redis keys: {keys[:20]}")
        server = client.info("server")
        memory = client.info("memory")
        stats = client.info("stats")
        cpu = client.info("cpu")
        return {
            "status": "pass",
            "redis_version": server["redis_version"],
            "port": fixture.port,
            "load_seconds": options.load_seconds,
            "selector_fanout": options.selector_fanout,
            "scale_registrations": options.scale_registrations,
            "used_memory_peak_bytes": memory["used_memory_peak"],
            "total_commands_processed": stats["total_commands_processed"],
            "total_net_input_bytes": stats["total_net_input_bytes"],
            "total_net_output_bytes": stats["total_net_output_bytes"],
            "redis_cpu_user_seconds": cpu["used_cpu_user"],
            "redis_cpu_system_seconds": cpu["used_cpu_sys"],
            "suites": results,
        }
    finally:
        client.close()


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="192.168.0.119")
    parser.add_argument("--ssh-user", default="ubuntu")
    parser.add_argument("--port", type=int, default=16380)
    parser.add_argument("--load-seconds", type=int, default=120)
    parser.add_argument("--selector-fanout", type=int, default=8)
    parser.add_argument("--scale-registrations", type=int, default=5_000)
    parser.add_argument("--skip-load", action="store_true")
    parser.add_argument("--keep-container", action="store_true")
    parser.add_argument(
        "--result-file",
        help="optional path receiving the complete JSON result",
    )
    parser.add_argument(
        "--ssh-password-env",
        default="VERDANDI_TEST_SSH_PASSWORD",
        help="environment variable containing the SSH password",
    )
    options = parser.parse_args()
    if not 1 <= options.port <= 65_535:
        parser.error("--port must be 1..65535")
    if not 1 <= options.load_seconds <= 3_600:
        parser.error("--load-seconds must be 1..3600")
    if not 1 <= options.selector_fanout <= 64:
        parser.error("--selector-fanout must be 1..64")
    if not 1 <= options.scale_registrations <= 100_000:
        parser.error("--scale-registrations must be 1..100000")
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
    try:
        fixture.deploy()
        result = qualify(fixture, options)
        serialized = json.dumps(result, indent=2, sort_keys=True)
        if options.result_file:
            target = Path(options.result_file).resolve()
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(serialized + "\n", encoding="utf-8")
        print(serialized)
        return 0
    except Exception as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    finally:
        if not options.keep_container:
            fixture.cleanup()
        remote.close()


if __name__ == "__main__":
    raise SystemExit(main())
