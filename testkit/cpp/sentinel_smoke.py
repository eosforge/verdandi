#!/usr/bin/env python3
"""Run the C++ SDK's short integration suite through an isolated Sentinel topology."""

from __future__ import annotations

import argparse
import json
import os
import secrets
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPOSITORY))

from testkit.sentinel.sentinel_test import (  # noqa: E402
    MASTER_NAME,
    SENTINEL_PORTS,
    TLS_SERVER_NAME,
    Credentials,
    Remote,
    TLSMaterial,
    Topology,
)


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="192.168.0.90")
    parser.add_argument("--ssh-user", default="ubuntu")
    parser.add_argument("--ssh-password-env", default="VERDANDI_TEST_SSH_PASSWORD")
    parser.add_argument("--build", default="gcc-debug")
    parser.add_argument("--runtime", choices=("linux-x64", "win-x64"), default="linux-x64")
    parser.add_argument("--result-file")
    parser.add_argument("--keep-topology", action="store_true")
    parser.add_argument("--tls", action="store_true")
    return parser.parse_args()


def wsl_path(path: Path) -> str:
    completed = subprocess.run(
        ["wsl.exe", "--", "wslpath", "-a", path.as_posix()],
        check=True,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    return completed.stdout.strip()


def run_client(
    repository: Path,
    build: str,
    runtime: str,
    environment: dict[str, str],
    *,
    expect_success: bool = True,
) -> None:
    if runtime == "win-x64":
        if os.name != "nt":
            raise RuntimeError("win-x64 C++ Sentinel tests require a Windows host")
        executable = repository / "sdk" / "cpp" / "build" / build / "Release" / "verdandi_cpp_redis_tests.exe"
        if not executable.is_file():
            raise RuntimeError(f"C++ Sentinel client is missing: {executable}")
        native_environment = {**os.environ, **environment}
        yyjson_directory = executable.parents[1] / "_deps" / "yyjson-build" / "Release"
        native_environment["PATH"] = os.pathsep.join(
            (str(executable.parent), str(yyjson_directory), native_environment.get("PATH", ""))
        )
        completed = subprocess.run([str(executable)], check=False, timeout=90, env=native_environment)
        if (completed.returncode == 0) != expect_success:
            raise RuntimeError(f"C++ Sentinel client exit {completed.returncode}, expected success={expect_success}")
        return

    executable = repository / "sdk" / "cpp" / "build" / build / "verdandi_cpp_redis_tests"
    if os.name == "nt":
        wsl_environment = environment.copy()
        if ca_file := wsl_environment.get("VERDANDI_TLS_CA_FILE"):
            wsl_environment["VERDANDI_TLS_CA_FILE"] = wsl_path(Path(ca_file))
        command = [
            "wsl.exe",
            "--cd",
            wsl_path(repository),
            "--",
            "env",
            *(f"{name}={value}" for name, value in wsl_environment.items()),
            f"sdk/cpp/build/{build}/verdandi_cpp_redis_tests",
        ]
        completed = subprocess.run(command, check=False, timeout=90)
        if (completed.returncode == 0) != expect_success:
            raise RuntimeError(f"C++ Sentinel client exit {completed.returncode}, expected success={expect_success}")
        return
    completed = subprocess.run([str(executable)], check=False, timeout=90, env={**os.environ, **environment})
    if (completed.returncode == 0) != expect_success:
        raise RuntimeError(f"C++ Sentinel client exit {completed.returncode}, expected success={expect_success}")


def main() -> int:
    options = arguments()
    password = os.environ.get(options.ssh_password_env)
    if not password:
        print(f"missing {options.ssh_password_env}", file=sys.stderr)
        return 2

    run_id = secrets.token_hex(4)
    credentials = Credentials.generate()
    remote = Remote(options.host, options.ssh_user, password)
    tls = TLSMaterial.generate() if options.tls else None
    topology = Topology(remote, run_id, credentials, tls=tls)
    started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="verdandi-cpp-sentinel-tls-", ignore_cleanup_errors=True) as temporary:
        try:
            topology.deploy()
            environment = {
                "VERDANDI_SENTINEL_ADDRS": ",".join(f"{options.host}:{port}" for port in SENTINEL_PORTS),
                "VERDANDI_SENTINEL_MASTER": MASTER_NAME,
                "VERDANDI_REDIS_USERNAME": "verdandi",
                "VERDANDI_REDIS_PASSWORD": credentials.app,
                "VERDANDI_SENTINEL_USERNAME": "sentinel-client",
                "VERDANDI_SENTINEL_PASSWORD": credentials.sentinel_client,
            }
            if tls is not None:
                ca_file = Path(temporary) / "ca.crt"
                ca_file.write_text(tls.ca_certificate, encoding="ascii")
                environment.update(
                    {
                        "VERDANDI_TLS_CA_FILE": str(ca_file),
                        "VERDANDI_TLS_SERVER_NAME": TLS_SERVER_NAME,
                    }
                )
                wrong_identity = environment.copy()
                wrong_identity["VERDANDI_TLS_SERVER_NAME"] = "wrong.verdandi.test"
                run_client(REPOSITORY, options.build, options.runtime, wrong_identity, expect_success=False)
            run_client(REPOSITORY, options.build, options.runtime, environment)
            master = topology.master_port()
            if topology.redis_cli(master, "DBSIZE").strip() != "0":
                raise RuntimeError("C++ Sentinel smoke left owned Redis keys")
            result = {
                "status": "pass",
                "scope": "C++23 root, Registration, Selector, Catalog and checkpoint through Sentinel",
                "client_runtime": options.runtime,
                "native_build": options.build,
                "redis_version": "8.8.0",
                "sentinels": len(SENTINEL_PORTS),
                "master_port": master,
                "tls": options.tls,
                "fixed_server_name": TLS_SERVER_NAME if options.tls else None,
                "wrong_identity_rejected": options.tls,
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
            if not options.keep_topology:
                topology.cleanup()
            remote.close()


if __name__ == "__main__":
    raise SystemExit(main())
