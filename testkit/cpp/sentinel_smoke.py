#!/usr/bin/env python3
"""Run the C++ SDK's short integration suite through an isolated Sentinel topology."""

from __future__ import annotations

import argparse
import json
import os
import secrets
import subprocess
import sys
import time
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPOSITORY))

from testkit.sentinel.sentinel_test import (  # noqa: E402
    MASTER_NAME,
    SENTINEL_PORTS,
    Credentials,
    Remote,
    Topology,
)


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="192.168.0.90")
    parser.add_argument("--ssh-user", default="ubuntu")
    parser.add_argument("--ssh-password-env", default="VERDANDI_TEST_SSH_PASSWORD")
    parser.add_argument("--build", default="gcc-debug")
    parser.add_argument("--result-file")
    parser.add_argument("--keep-topology", action="store_true")
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


def run_client(repository: Path, build: str, environment: dict[str, str]) -> None:
    executable = repository / "sdk" / "cpp" / "build" / build / "verdandi_cpp_redis_tests"
    if os.name == "nt":
        command = [
            "wsl.exe",
            "--cd",
            wsl_path(repository),
            "--",
            "env",
            *(f"{name}={value}" for name, value in environment.items()),
            f"sdk/cpp/build/{build}/verdandi_cpp_redis_tests",
        ]
        subprocess.run(command, check=True, timeout=90)
        return
    subprocess.run([str(executable)], check=True, timeout=90, env={**os.environ, **environment})


def main() -> int:
    options = arguments()
    password = os.environ.get(options.ssh_password_env)
    if not password:
        print(f"missing {options.ssh_password_env}", file=sys.stderr)
        return 2

    run_id = secrets.token_hex(4)
    credentials = Credentials.generate()
    remote = Remote(options.host, options.ssh_user, password)
    topology = Topology(remote, run_id, credentials)
    started = time.monotonic()
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
        run_client(REPOSITORY, options.build, environment)
        master = topology.master_port()
        if topology.redis_cli(master, "DBSIZE").strip() != "0":
            raise RuntimeError("C++ Sentinel smoke left owned Redis keys")
        result = {
            "status": "pass",
            "scope": "C++23 root, Registration, Selector, Catalog and checkpoint through Sentinel",
            "redis_version": "8.8.0",
            "sentinels": len(SENTINEL_PORTS),
            "master_port": master,
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
