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

from testkit.support import temporary_directory
from testkit.support import popen, stop_process
from testkit.suites import native_environment

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
    parser.add_argument("--host", default="192.168.0.119")
    parser.add_argument("--ssh-user", default="ubuntu")
    parser.add_argument("--ssh-password-env", default="VERDANDI_TEST_SSH_PASSWORD")
    parser.add_argument("--build", default="gcc-debug")
    parser.add_argument("--runtime", choices=("linux-x64", "win-x64"), default="win-x64" if os.name == "nt" else "linux-x64")
    parser.add_argument("--result-file")
    parser.add_argument("--keep-topology", action="store_true")
    parser.add_argument("--tls", action="store_true")
    return parser.parse_args()


def run_client(repository, build, runtime, environment, *, expect_success=True):
    expected = "win-x64" if os.name == "nt" else "linux-x64"
    if runtime != expected:
        raise RuntimeError("Use testkit/run.py to select the Linux VM")
    directory = Path(build)
    if not directory.is_absolute():
        directory = repository / "sdk/cpp/build" / build
    name = "verdandi_cpp_redis_tests.exe" if os.name == "nt" else "verdandi_cpp_redis_tests"
    executable = next((p for p in (directory / name, directory / "Debug" / name, directory / "Release" / name) if p.is_file()), None)
    if executable is None:
        raise RuntimeError(f"C++ Sentinel executable missing in {directory}")
    env, _ = native_environment()
    env.update(environment)
    process = popen([str(executable)], repository, env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        output, _ = process.communicate(timeout=90)
        code = process.returncode
        if code != (0 if expect_success else 1):
            raise RuntimeError(f"C++ Sentinel exit {code}, expected success={expect_success}: {output[-8192:]}")
        if not expect_success:
            print("PASS expected TLS identity rejection (normal error return, no native abort)", flush=True)
        elif output:
            print(output, end="", flush=True)
    finally:
        stop_process(process)


def main() -> int:
    options = arguments()
    password = os.environ.get(options.ssh_password_env)
    if not password:
        print(f"missing {options.ssh_password_env}", file=sys.stderr)
        return 2

    run_id = secrets.token_hex(4)
    credentials = Credentials.generate()
    remote = Remote(options.host, options.ssh_user, password)
    options.host = remote.host
    tls = TLSMaterial.generate() if options.tls else None
    topology = Topology(remote, run_id, credentials, tls=tls)
    started = time.monotonic()
    with temporary_directory(prefix="verdandi-cpp-sentinel-tls-") as temporary:
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
