#!/usr/bin/env python3
"""Run the C# facade through two Redis Sentinel promotions."""

from __future__ import annotations

import argparse
import contextlib
import json
import os
import secrets
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parents[3]
CSHARP = REPOSITORY / "sdk" / "csharp"
CPP = REPOSITORY / "sdk" / "cpp"
sys.path.insert(0, str(REPOSITORY))

from sdk.csharp.tests.standalone_test import build_native as build_linux_native  # noqa: E402
from testkit.sentinel.sentinel_test import (  # noqa: E402
    MASTER_NAME,
    REDIS_PORTS,
    SENTINEL_PORTS,
    TLS_SERVER_NAME,
    Credentials,
    Peer,
    QualificationError,
    Remote,
    TLSMaterial,
    Topology,
    parse_ready,
)
from testkit.standalone.standalone_test import run_command  # noqa: E402


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="192.168.0.90")
    parser.add_argument("--ssh-user", default="ubuntu")
    parser.add_argument("--ssh-password-env", default="VERDANDI_TEST_SSH_PASSWORD")
    parser.add_argument("--runtime", choices=("linux-x64", "win-x64"), default="linux-x64")
    parser.add_argument("--vcpkg-root")
    parser.add_argument("--result-file")
    parser.add_argument("--keep-topology", action="store_true")
    parser.add_argument("--tls", action="store_true")
    return parser.parse_args()


def alphabetic_zone() -> str:
    alphabet = "ABCDEFGHIJKLMNOP"
    return "CSharpSentinel" + "".join(alphabet[value & 15] for value in secrets.token_bytes(10))


def configuration(host: str, credentials: Credentials, zone: str, tls: bool) -> str:
    redis = {
        "mode": "sentinel",
        "addresses": [f"{host}:{port}" for port in SENTINEL_PORTS],
        "master_name": MASTER_NAME,
        "auth": {"username": "verdandi", "password": credentials.app},
        "sentinel_auth": {
            "username": "sentinel-client",
            "password": credentials.sentinel_client,
        },
        "timeout_ms": 1_000,
        "connect_timeout_ms": 250,
        "reconnect": {"delay_ms": 10},
    }
    if tls:
        redis["tls"] = {
            "enabled": True,
            "system_roots": False,
            "server_name": TLS_SERVER_NAME,
            "ca_file": "ca.crt",
        }
    return json.dumps(
        {
            "version": "v1",
            "redis": redis,
            "registration": {
                "zone": zone,
                "selector": {"sync_timeout_ms": 5_000},
            },
            "catalog": {
                "zone": zone,
                "sync_timeout_ms": 5_000,
                "max_record_bytes": 4 * 1024 * 1024,
            },
        },
        separators=(",", ":"),
    )


def build_native(runtime: str, vcpkg_root: str | None) -> list[dict[str, object]]:
    if runtime == "linux-x64":
        return build_linux_native()
    if os.name != "nt":
        raise QualificationError("win-x64 C# Sentinel tests require a Windows host")

    root = Path(vcpkg_root or os.environ.get("VCPKG_ROOT", ""))
    toolchain = root / "scripts" / "buildsystems" / "vcpkg.cmake"
    if not toolchain.is_file():
        raise QualificationError("--vcpkg-root must identify an existing vcpkg checkout for win-x64")

    build = CPP / "build" / "msvc-shared-release"
    commands = (
        (
            "C# native Windows Release configure",
            [
                "cmake",
                "-S",
                str(CPP),
                "-B",
                str(build),
                "-G",
                "Visual Studio 18 2026",
                "-A",
                "x64",
                f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
                "-DBUILD_SHARED_LIBS=ON",
                "-DVERDANDI_BUILD_TESTS=ON",
            ],
        ),
        (
            "C# native Windows Release build",
            ["cmake", "--build", str(build), "--config", "Release"],
        ),
    )
    return [run_command(name, command, CPP, os.environ.copy()) for name, command in commands]


def publish_peers(root: Path, config: str, tls: TLSMaterial | None, runtime: str) -> dict[str, Path]:
    if runtime == "win-x64":
        native_directory = CPP / "build" / "msvc-shared-release" / "Release"
        native_libraries = (
            native_directory / "verdandi_cpp.dll",
            CPP / "build" / "msvc-shared-release" / "_deps" / "yyjson-build" / "Release" / "yyjson.dll",
            native_directory / "libssl-3-x64.dll",
            native_directory / "libcrypto-3-x64.dll",
        )
    else:
        native_libraries = (CPP / "build" / "gcc-shared-release" / "libverdandi_cpp.so",)
    for native in native_libraries:
        if not native.is_file():
            raise QualificationError(f"native Release runtime is missing: {native}")

    outputs: dict[str, Path] = {}
    for framework in ("net8.0", "net10.0"):
        output = root / framework
        run_command(
            f"C# Sentinel peer publish {framework}",
            [
                "dotnet",
                "publish",
                "tests/Verdandi.Tests/Verdandi.Tests.csproj",
                "--configuration",
                "Release",
                "--framework",
                framework,
                "--runtime",
                runtime,
                "--self-contained",
                "true",
                "--output",
                str(output),
            ],
            CSHARP,
            os.environ.copy(),
        )
        for native in native_libraries:
            shutil.copy2(native, output / native.name)
        (output / "configuration.json").write_text(config, encoding="utf-8")
        if tls is not None:
            (output / "ca.crt").write_text(tls.ca_certificate, encoding="ascii")
        outputs[framework] = output
    return outputs


def peer_command(output: Path, runtime: str, configuration_file: str = "configuration.json") -> tuple[list[str], Path, dict[str, str]]:
    environment = os.environ.copy()
    environment.pop("VERDANDI_TEST_SSH_PASSWORD", None)
    if runtime == "win-x64":
        if os.name != "nt":
            raise QualificationError("win-x64 C# Sentinel peers require a Windows host")
        environment["VERDANDI_NATIVE_LIBRARY"] = "./verdandi_cpp.dll"
        environment["PATH"] = os.pathsep.join((str(output), environment.get("PATH", "")))
        return (
            [str(output / "Verdandi.Tests.exe"), "--peer", "--configuration-file", configuration_file],
            output,
            environment,
        )
    if os.name == "nt":
        return (
            [
                "wsl.exe",
                "--cd",
                str(output),
                "--",
                "env",
                "LD_LIBRARY_PATH=.",
                "VERDANDI_NATIVE_LIBRARY=./libverdandi_cpp.so",
                "./Verdandi.Tests",
                "--peer",
                "--configuration-file",
                configuration_file,
            ],
            REPOSITORY,
            environment,
        )
    environment.update(
        {
            "LD_LIBRARY_PATH": ".",
            "VERDANDI_NATIVE_LIBRARY": "./libverdandi_cpp.so",
        }
    )
    return (
        ["./Verdandi.Tests", "--peer", "--configuration-file", configuration_file],
        output,
        environment,
    )


def revision(line: str) -> int:
    parts = line.split()
    if len(parts) != 2 or not parts[1].isdigit():
        raise QualificationError(f"invalid peer revision line: {line}")
    return int(parts[1])


def generation(line: str) -> int:
    parts = line.split()
    if len(parts) != 2 or not parts[1].isdigit():
        raise QualificationError(f"invalid peer generation line: {line}")
    return int(parts[1])


def registration_key(zone: str, uuid: str) -> str:
    return f"verdandi:registration:{zone}:Sentinel:{uuid}"


def send_all(peers: list[Peer], command: str, expected: str, timeout: float = 45) -> list[str]:
    return [peer.send(command, expected, timeout) for peer in peers]


def close_peers(peers: list[Peer]) -> None:
    for peer in peers:
        peer.close()


def reject_wrong_identity(output: Path, runtime: str, config: str) -> None:
    document = json.loads(config)
    document["redis"]["tls"]["server_name"] = "wrong.verdandi.test"
    (output / "wrong-configuration.json").write_text(
        json.dumps(document, separators=(",", ":")),
        encoding="utf-8",
    )
    command, directory, environment = peer_command(output, runtime, "wrong-configuration.json")
    completed = subprocess.run(
        command,
        cwd=directory,
        env=environment,
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=30,
    )
    if completed.returncode == 0:
        raise QualificationError("C# Sentinel peer accepted the wrong fixed TLS certificate identity")


def widen_sentinel_replica_window(topology: Topology) -> None:
    for name, port in zip(topology.sentinels, SENTINEL_PORTS, strict=True):
        command = (
            f"docker exec {shlex.quote(name)} redis-cli {topology.cli_tls_arguments()} -p {port} "
            f"--user sentinel-admin --pass {shlex.quote(topology.credentials.sentinel_admin)} "
            f"--no-auth-warning --raw SENTINEL SET {MASTER_NAME} down-after-milliseconds 5000"
        )
        if topology.remote.run(command).strip() != "OK":
            raise QualificationError(f"failed to configure C# Sentinel eligibility window on port {port}")


def print_topology_diagnostics(topology: Topology) -> None:
    print("C# Sentinel topology diagnostics:", file=sys.stderr)
    for name, port in zip(topology.sentinels, SENTINEL_PORTS, strict=True):
        if not topology.is_running(name):
            print(f"{name}: stopped", file=sys.stderr)
            continue
        prefix = (
            f"docker exec {shlex.quote(name)} redis-cli {topology.cli_tls_arguments()} -p {port} "
            f"--user sentinel-client --pass {shlex.quote(topology.credentials.sentinel_client)} "
            "--no-auth-warning --raw"
        )
        master = topology.remote.run(f"{prefix} SENTINEL MASTER {MASTER_NAME}", check=False)
        replicas = topology.remote.run(f"{prefix} SENTINEL REPLICAS {MASTER_NAME}", check=False)
        logs = topology.remote.run(f"docker logs --tail 40 {shlex.quote(name)}", check=False)
        print(f"{name} MASTER:\n{master}", file=sys.stderr)
        print(f"{name} REPLICAS:\n{replicas}", file=sys.stderr)
        print(f"{name} LOGS:\n{logs}", file=sys.stderr)
    for name, port in zip(topology.redis, REDIS_PORTS, strict=True):
        if topology.is_running(name):
            print(f"{name} ROLE:\n{topology.role(port)}", file=sys.stderr)


def main() -> int:
    options = arguments()
    password = os.environ.get(options.ssh_password_env)
    if not password:
        print(f"missing {options.ssh_password_env}", file=sys.stderr)
        return 2

    started = time.monotonic()
    run_id = secrets.token_hex(4)
    credentials = Credentials.generate()
    zone = alphabetic_zone()
    remote = Remote(options.host, options.ssh_user, password)
    tls = TLSMaterial.generate() if options.tls else None
    topology = Topology(remote, run_id, credentials, tls=tls)
    peers: list[Peer] = []
    try:
        native_suites = build_native(options.runtime, options.vcpkg_root)
        with contextlib.ExitStack() as resources:
            temporary = resources.enter_context(tempfile.TemporaryDirectory(prefix="verdandi-csharp-sentinel-", ignore_cleanup_errors=True))
            resources.callback(close_peers, peers)
            config = configuration(options.host, credentials, zone, options.tls)
            outputs = publish_peers(Path(temporary), config, tls, options.runtime)
            topology.deploy()
            widen_sentinel_replica_window(topology)
            if options.tls:
                reject_wrong_identity(next(iter(outputs.values())), options.runtime, config)
            for framework, output in outputs.items():
                command, directory, environment = peer_command(output, options.runtime)
                peers.append(Peer(framework, command, directory, environment))

            uuids = [parse_ready(peer.read("READY")) for peer in peers]
            initial_generations = [generation(line) for line in send_all(peers, "CHECK", "CHECKED")]
            send_all(peers, "UPDATE 10", "UPDATED")
            send_all(peers, "CHECK", "CHECKED")

            old_master = topology.master_port()
            topology.stop_sentinel(2)
            replica_indices = [index for index, port in enumerate(REDIS_PORTS) if port != old_master]
            for index in replica_indices:
                topology.pause_redis(index)
            topology.redis_cli(old_master, "CLIENT", "KILL", "TYPE", "REPLICA")
            lost_revisions = [revision(line) for line in send_all(peers, "UPDATE 20", "UPDATED")]
            topology.kill_redis_port(old_master)
            promoted_index = max(replica_indices)
            topology.unpause_redis(promoted_index)
            new_master = topology.wait_master(old_master, timeout=25)
            expected_master = REDIS_PORTS[promoted_index]
            if new_master != expected_master:
                raise QualificationError(f"Sentinel promoted {new_master}, expected deterministic candidate {expected_master}")
            for index in replica_indices:
                if index != promoted_index:
                    topology.unpause_redis(index)
            for uuid, lost_revision in zip(uuids, lost_revisions, strict=True):
                stale = topology.redis_cli(new_master, "HGET", registration_key(zone, uuid), "@revision").strip()
                if stale and (not stale.isdigit() or int(stale) >= lost_revision):
                    raise QualificationError(f"C# acknowledged write was not demonstrably lost: observed={stale!r}, acknowledged={lost_revision}")

            send_all(peers, "RENEW", "RENEWED", 60)
            recovered_generations = [generation(line) for line in send_all(peers, "CHECK", "CHECKED", 60)]
            if any(current <= initial for current, initial in zip(recovered_generations, initial_generations, strict=True)):
                raise QualificationError("C# Selector generation did not advance after failover")

            topology.start_sentinel(2)
            topology.wait_sentinel_agreement(new_master)
            surviving_replica = next(port for port in REDIS_PORTS if port not in {old_master, new_master})
            topology.wait_replica_ready(new_master, surviving_replica)
            topology.redis_cli(new_master, "SCRIPT", "FLUSH")
            send_all(peers, "UPDATE 30", "UPDATED")
            send_all(peers, "CHECK", "CHECKED")

            for index in range(3):
                topology.stop_sentinel(index)
            send_all(peers, "UPDATE 40", "UPDATED")
            send_all(peers, "CHECK", "CHECKED")
            topology.kill_redis_port(new_master)
            send_all(peers, "WAIT_UNSYNC", "UNSYNCHRONIZED", 30)

            for index in range(3):
                topology.start_sentinel(index)
            final_master = topology.wait_master(new_master, timeout=60)
            send_all(peers, "RENEW", "RENEWED", 60)
            final_generations = [generation(line) for line in send_all(peers, "CHECK", "CHECKED", 60)]
            if any(current <= recovered for current, recovered in zip(final_generations, recovered_generations, strict=True)):
                raise QualificationError("C# Selector generation did not advance after total Sentinel loss")

            send_all(peers, "UPDATE 50", "UPDATED")
            send_all(peers, "CHECK", "CHECKED")
            send_all(peers, "STOP", "STOPPED")
            topology.redis_cli(final_master, "DEL", f"verdandi:config:{zone}")
            if topology.redis_cli(final_master, "DBSIZE").strip() != "0":
                raise QualificationError("C# Sentinel regression left Redis keys")

            result = {
                "status": "pass",
                "language": "C#",
                "frameworks": list(outputs),
                "client_runtime": options.runtime,
                "redis_version": "8.8.0",
                "tls": options.tls,
                "fixed_server_name": TLS_SERVER_NAME if options.tls else None,
                "wrong_identity_rejected": options.tls,
                "initial_master": old_master,
                "acknowledged_loss_master": new_master,
                "recovered_master": final_master,
                "selector_generations": {
                    framework: [initial, recovered, final]
                    for framework, initial, recovered, final in zip(
                        outputs,
                        initial_generations,
                        recovered_generations,
                        final_generations,
                        strict=True,
                    )
                },
                "scenarios": [
                    "private-CA Sentinel and data-plane TLS with one fixed certificate identity" if options.tls else "plain Sentinel transport",
                    "separate Redis and Sentinel ACL credentials",
                    "two managed target frameworks in one topology",
                    "forced acknowledged write loss and desired-state repair",
                    "Pub/Sub generation recovery",
                    "SCRIPT FLUSH reload",
                    "all Sentinels unavailable",
                    "primary loss while all Sentinels are unavailable",
                    "Sentinel restart and second promotion",
                ],
                "native_suites": [
                    {
                        "name": suite["name"],
                        "status": suite["status"],
                        "elapsed_seconds": suite["elapsed_seconds"],
                    }
                    for suite in native_suites
                ],
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
        print_topology_diagnostics(topology)
        return 1
    finally:
        close_peers(peers)
        if not options.keep_topology:
            topology.cleanup()
        remote.close()


if __name__ == "__main__":
    raise SystemExit(main())
