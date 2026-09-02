#!/usr/bin/env python3
"""Run the complete C# facade regression against an owned Redis 8 fixture."""

from __future__ import annotations

import argparse
import json
import os
import secrets
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import redis

REPOSITORY = Path(__file__).resolve().parents[3]
CSHARP = REPOSITORY / "sdk" / "csharp"
CPP = REPOSITORY / "sdk" / "cpp"
sys.path.insert(0, str(REPOSITORY))

from testkit.sentinel.sentinel_test import QualificationError, Remote  # noqa: E402
from testkit.standalone.standalone_test import Fixture, run_command  # noqa: E402


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="192.168.0.90")
    parser.add_argument("--ssh-user", default="ubuntu")
    parser.add_argument("--port", type=int, default=16384)
    parser.add_argument("--ssh-password-env", default="VERDANDI_TEST_SSH_PASSWORD")
    parser.add_argument("--result-file")
    parser.add_argument("--keep-container", action="store_true")
    options = parser.parse_args()
    if not 1 <= options.port <= 65_535:
        parser.error("--port must be 1..65535")
    return options


def alphabetic_zone(prefix: str) -> str:
    alphabet = "ABCDEFGHIJKLMNOP"
    return prefix + "".join(alphabet[value & 15] for value in secrets.token_bytes(10))


def configuration(fixture: Fixture) -> str:
    return json.dumps(
        {
            "version": "v1",
            "redis": {
                "mode": "standalone",
                "addresses": [f"{fixture.remote.host}:{fixture.port}"],
                "auth": {"username": "default", "password": fixture.password},
            },
            "registration": {
                "zone": alphabetic_zone("CSRegistration"),
                "selector": {"sync_timeout_ms": 5_000},
            },
            "catalog": {
                "zone": alphabetic_zone("CSCatalog"),
                "sync_timeout_ms": 5_000,
                "max_record_bytes": 4 * 1024 * 1024,
            },
        },
        separators=(",", ":"),
    )


def run_linux(
    name: str,
    command: list[str],
    directory: Path,
    environment: dict[str, str],
    required_output: str | None = None,
) -> dict[str, object]:
    if os.name != "nt":
        return run_command(name, command, directory, {**os.environ, **environment}, required_output)

    forwarded = [f"{key}={value}" for key, value in environment.items()]
    return run_command(
        name,
        ["wsl.exe", "--cd", str(directory), "--", "env", *forwarded, *command],
        REPOSITORY,
        os.environ.copy(),
        required_output,
    )


def build_native() -> list[dict[str, object]]:
    commands = (
        ("C# native Release configure", ["cmake", "--preset", "gcc-shared-release"]),
        ("C# native Release build", ["cmake", "--build", "--preset", "gcc-shared-release"]),
    )
    return [run_linux(name, command, CPP, {}) for name, command in commands]


def host_runtime_environment() -> dict[str, str] | None:
    if os.name == "nt":
        runtime = CPP / "build" / "msvc-shared-release" / "Release" / "verdandi_cpp.dll"
        if not runtime.is_file():
            return None
        return {
            **os.environ,
            "VERDANDI_NATIVE_LIBRARY": str(runtime),
            "PATH": str(runtime.parent) + os.pathsep + os.environ.get("PATH", ""),
        }

    runtime = CPP / "build" / "gcc-shared-release" / "libverdandi_cpp.so"
    if not runtime.is_file():
        return None
    return {
        **os.environ,
        "VERDANDI_NATIVE_LIBRARY": str(runtime),
        "LD_LIBRARY_PATH": str(runtime.parent) + os.pathsep + os.environ.get("LD_LIBRARY_PATH", ""),
    }


def run_managed(fixture: Fixture) -> list[dict[str, object]]:
    results = [
        run_command(
            "C# restore",
            ["dotnet", "restore", "Verdandi.slnx"],
            CSHARP,
            os.environ.copy(),
        ),
        run_command(
            "C# format and analyzer gate",
            ["dotnet", "format", "Verdandi.slnx", "--verify-no-changes", "--no-restore"],
            CSHARP,
            os.environ.copy(),
        ),
        run_command(
            "C# .NET 8 and .NET 10 Release build",
            ["dotnet", "build", "Verdandi.slnx", "--configuration", "Release", "--no-restore"],
            CSHARP,
            os.environ.copy(),
        ),
    ]

    host_environment = host_runtime_environment()
    if host_environment is None:
        results.append(
            {
                "name": "C# host offline regression",
                "status": "skipped",
                "elapsed_seconds": 0.0,
                "output": (
                    "No matching host native runtime; both Linux self-contained "
                    "integrations still run the offline suite."
                ),
            }
        )
    else:
        for framework in ("net8.0", "net10.0"):
            results.append(
                run_command(
                    f"C# {framework} offline regression",
                    [
                        "dotnet",
                        "run",
                        "--project",
                        "tests/Verdandi.Tests",
                        "--framework",
                        framework,
                        "--configuration",
                        "Release",
                        "--no-build",
                    ],
                    CSHARP,
                    host_environment,
                    required_output="Verdandi C# offline tests passed.",
                )
            )

    native = CPP / "build" / "gcc-shared-release" / "libverdandi_cpp.so"
    if not native.is_file():
        raise QualificationError(f"native Release runtime is missing: {native}")

    with tempfile.TemporaryDirectory(prefix="verdandi-csharp-") as temporary:
        temporary_root = Path(temporary)
        for framework, explicit in (("net8.0", True), ("net10.0", False)):
            output = temporary_root / framework
            results.append(
                run_command(
                    f"C# {framework} self-contained Linux publish",
                    [
                        "dotnet",
                        "publish",
                        "tests/Verdandi.Tests/Verdandi.Tests.csproj",
                        "--configuration",
                        "Release",
                        "--framework",
                        framework,
                        "--runtime",
                        "linux-x64",
                        "--self-contained",
                        "true",
                        "--output",
                        str(output),
                    ],
                    CSHARP,
                    os.environ.copy(),
                )
            )
            shutil.copy2(native, output / native.name)
            (output / "configuration.json").write_text(configuration(fixture), encoding="utf-8")
            environment = {
                "LD_LIBRARY_PATH": ".",
            }
            if explicit:
                environment["VERDANDI_NATIVE_LIBRARY"] = "./libverdandi_cpp.so"
            results.append(
                run_linux(
                    f"C# {framework} ACL Redis integration ({'explicit' if explicit else 'application-directory'} loader)",
                    ["./Verdandi.Tests", "--configuration-file", "configuration.json"],
                    output,
                    environment,
                    required_output="Verdandi C# offline and Redis tests passed.",
                )
            )
    return results


def main() -> int:
    options = arguments()
    password = os.environ.get(options.ssh_password_env)
    if not password:
        print(f"missing {options.ssh_password_env}", file=sys.stderr)
        return 2

    run_id = secrets.token_hex(4)
    remote = Remote(options.host, options.ssh_user, password)
    fixture = Fixture(remote, run_id, options.port)
    started = time.monotonic()
    try:
        fixture.deploy()
        suites = [*build_native(), *run_managed(fixture)]
        client = redis.Redis.from_url(fixture.url)
        try:
            keys = sorted(key.decode(errors="replace") for key in client.scan_iter())
            if keys:
                raise QualificationError(f"successful C# suites left Redis keys: {keys[:20]}")
            server = client.info("server")
        finally:
            client.close()

        result = {
            "status": "pass",
            "language": "C#",
            "frameworks": ["net8.0", "net10.0"],
            "runtime": "linux-x64",
            "redis_version": server["redis_version"],
            "native_build": "gcc-shared-release",
            "suites": suites,
            "elapsed_seconds": round(time.monotonic() - started, 3),
        }
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
