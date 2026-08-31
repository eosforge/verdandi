#!/usr/bin/env python3
"""Qualify Verdandi against an isolated Redis 8 Sentinel topology.

The harness uses host networking with dedicated ports so Sentinel and Redis
announce exactly the addresses seen by SDK clients. It owns only containers
and a remote /tmp directory bearing its random run identifier.
"""

from __future__ import annotations

import argparse
import json
import os
import queue
import re
import secrets
import shlex
import socket
import subprocess
import sys
import tempfile
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable
from urllib.parse import quote

import paramiko

REDIS_PORTS = (16381, 16382, 16383)
SENTINEL_PORTS = (26381, 26382, 26383)
MASTER_NAME = "verdandi-primary"
TYPE_NAME = "fault"


class QualificationError(RuntimeError):
    """One deterministic qualification assertion failed."""


class Remote:
    def __init__(self, host: str, username: str, password: str) -> None:
        self.host = host
        self._client = paramiko.SSHClient()
        self._client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
        self._client.connect(
            host,
            username=username,
            password=password,
            timeout=10,
            banner_timeout=10,
            auth_timeout=10,
        )

    def close(self) -> None:
        self._client.close()

    def run(self, command: str, *, check: bool = True) -> str:
        _, stdout, stderr = self._client.exec_command(command)
        output = stdout.read().decode("utf-8", errors="replace")
        diagnostics = stderr.read().decode("utf-8", errors="replace")
        status = stdout.channel.recv_exit_status()
        if check and status != 0:
            raise QualificationError(f"remote command failed ({status}): {command}\n{diagnostics}")
        return output

    def write(self, path: str, content: str) -> None:
        sftp = self._client.open_sftp()
        try:
            with sftp.file(path, "w") as target:
                target.write(content)
            sftp.chmod(path, 0o666)
        finally:
            sftp.close()


@dataclass(frozen=True)
class Credentials:
    admin: str
    app: str
    monitor: str
    replica: str
    sentinel_admin: str
    sentinel_client: str

    @classmethod
    def generate(cls) -> "Credentials":
        return cls(*(secrets.token_hex(16) for _ in range(6)))


class Topology:
    def __init__(self, remote: Remote, run_id: str, credentials: Credentials) -> None:
        if not re.fullmatch(r"[a-z0-9]{8}", run_id):
            raise QualificationError("invalid topology run identifier")
        self.remote = remote
        self.run_id = run_id
        self.credentials = credentials
        self.root = f"/tmp/verdandi-sentinel-it-{run_id}"
        self.redis = [f"verdandi-it-{run_id}-redis-{index}" for index in range(1, 4)]
        self.sentinels = [f"verdandi-it-{run_id}-sentinel-{index}" for index in range(1, 4)]
        self.paused: set[str] = set()

    def deploy(self) -> None:
        self._assert_ports_free((*REDIS_PORTS, *SENTINEL_PORTS))
        existing = set(self.remote.run("docker ps -a --format '{{.Names}}'").splitlines())
        collisions = existing.intersection((*self.redis, *self.sentinels))
        if collisions:
            raise QualificationError(f"container collision: {sorted(collisions)}")

        directories = [
            *(f"{self.root}/redis-{index}" for index in range(1, 4)),
            *(f"{self.root}/sentinel-{index}" for index in range(1, 4)),
        ]
        self.remote.run("mkdir -p " + " ".join(map(shlex.quote, directories)))
        self.remote.run("chmod 777 " + " ".join(map(shlex.quote, directories)))
        self._write_redis_configuration()
        self._write_sentinel_configuration()

        for index, name in enumerate(self.redis, 1):
            directory = f"{self.root}/redis-{index}"
            self.remote.run(
                "docker run -d "
                f"--name {shlex.quote(name)} --network host --tmpfs /data "
                f"--label verdandi.test={self.run_id} "
                f"-v {shlex.quote(directory)}:/test:ro "
                "redis:8.8.0 redis-server /test/redis.conf"
            )
        self._wait_ports(REDIS_PORTS)

        for index, name in enumerate(self.sentinels, 1):
            directory = f"{self.root}/sentinel-{index}"
            self.remote.run(
                "docker run -d "
                f"--name {shlex.quote(name)} --network host "
                f"--label verdandi.test={self.run_id} "
                f"-v {shlex.quote(directory)}:/test "
                "redis:8.8.0 redis-sentinel /test/sentinel.conf"
            )
        self._wait_ports(SENTINEL_PORTS)
        if self.wait_master(timeout=20) != REDIS_PORTS[0]:
            raise QualificationError("Sentinel did not retain the configured initial primary")
        self._wait_replicas(2)

    def cleanup(self) -> None:
        for name in self.paused.copy():
            self.remote.run(f"docker unpause {shlex.quote(name)}", check=False)
            self.paused.discard(name)
        for name in reversed((*self.redis, *self.sentinels)):
            self.remote.run(f"docker rm -f {shlex.quote(name)}", check=False)
        if not re.fullmatch(r"/tmp/verdandi-sentinel-it-[a-z0-9]{8}", self.root):
            raise QualificationError(f"refusing unsafe remote cleanup path {self.root!r}")
        self.remote.run(f"rm -rf -- {shlex.quote(self.root)}", check=False)

    def master_port(self) -> int:
        for name, port in zip(self.sentinels, SENTINEL_PORTS, strict=True):
            if not self.is_running(name):
                continue
            output = self.remote.run(
                f"docker exec {shlex.quote(name)} redis-cli -p {port} "
                f"--user sentinel-client --pass {self.credentials.sentinel_client} "
                "--no-auth-warning --raw SENTINEL get-master-addr-by-name "
                f"{MASTER_NAME}",
                check=False,
            ).splitlines()
            if len(output) >= 2 and output[-1].isdigit():
                return int(output[-1])
        raise QualificationError("no Sentinel returned a primary")

    def wait_master(self, different_from: int | None = None, *, timeout: float = 20) -> int:
        deadline = time.monotonic() + timeout
        last = "no Sentinel response"
        while time.monotonic() < deadline:
            try:
                port = self.master_port()
                if different_from is not None and port == different_from:
                    last = f"Sentinel still reports excluded primary {port}"
                    time.sleep(0.1)
                    continue
                role = self.role(port)
                if role and role[0] == "master":
                    return port
                last = f"Sentinel reports port {port} with ROLE {role[:2]}"
            except Exception as error:  # qualification polling retains the last cause
                last = repr(error)
            time.sleep(0.1)
        raise QualificationError(f"Sentinel did not publish a usable new primary; last error: {last}")

    def role(self, port: int) -> list[str]:
        return self.redis_cli(port, "ROLE").splitlines()

    def redis_cli(self, port: int, *arguments: str) -> str:
        control = self._control_container()
        command = [
            "docker",
            "exec",
            control,
            "redis-cli",
            "-h",
            "127.0.0.1",
            "-p",
            str(port),
            "--user",
            "admin",
            "--pass",
            self.credentials.admin,
            "--no-auth-warning",
            "--raw",
            *arguments,
        ]
        return self.remote.run(" ".join(map(shlex.quote, command)))

    def stop_sentinel(self, index: int) -> None:
        self.remote.run(f"docker stop {shlex.quote(self.sentinels[index])}")

    def start_sentinel(self, index: int) -> None:
        self.remote.run(f"docker start {shlex.quote(self.sentinels[index])}")
        self._wait_ports((SENTINEL_PORTS[index],))

    def pause_redis(self, index: int) -> None:
        name = self.redis[index]
        self.remote.run(f"docker pause {shlex.quote(name)}")
        self.paused.add(name)

    def unpause_redis(self, index: int) -> None:
        name = self.redis[index]
        self.remote.run(f"docker unpause {shlex.quote(name)}")
        self.paused.discard(name)

    def kill_redis_port(self, port: int) -> None:
        index = REDIS_PORTS.index(port)
        name = self.redis[index]
        self.remote.run(f"docker kill {shlex.quote(name)}")

    def is_running(self, name: str) -> bool:
        return (
            self.remote.run(
                f"docker inspect -f '{{{{.State.Running}}}}' {shlex.quote(name)}",
                check=False,
            ).strip()
            == "true"
        )

    def sentinel_monitor_line(self, index: int) -> str:
        directory = f"{self.root}/sentinel-{index + 1}"
        output = self.remote.run(
            "docker run --rm " f"-v {shlex.quote(directory)}:/test:ro redis:8.8.0 " f"grep '^sentinel monitor {MASTER_NAME} ' /test/sentinel.conf"
        )
        return output.strip()

    def wait_sentinel_agreement(self, expected_port: int, *, timeout: float = 15) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            ports: list[int] = []
            for name, port in zip(self.sentinels, SENTINEL_PORTS, strict=True):
                output = self.remote.run(
                    f"docker exec {shlex.quote(name)} redis-cli -p {port} "
                    f"--user sentinel-client --pass {self.credentials.sentinel_client} "
                    "--no-auth-warning --raw SENTINEL get-master-addr-by-name "
                    f"{MASTER_NAME}",
                    check=False,
                ).splitlines()
                if len(output) >= 2 and output[-1].isdigit():
                    ports.append(int(output[-1]))
            if ports == [expected_port, expected_port, expected_port]:
                return
            time.sleep(0.1)
        raise QualificationError("Sentinels did not converge on the promoted primary")

    def wait_replica_ready(self, master_port: int, replica_port: int, *, timeout: float = 20) -> None:
        deadline = time.monotonic() + timeout
        last = "replica topology was not observed"
        while time.monotonic() < deadline:
            try:
                role = self.role(replica_port)
                attached = len(role) >= 4 and role[0] in {"slave", "replica"} and role[2] == str(master_port) and role[3] == "connected"
                known_by: list[int] = []
                for name, port in zip(self.sentinels, SENTINEL_PORTS, strict=True):
                    output = self.remote.run(
                        f"docker exec {shlex.quote(name)} redis-cli -p {port} "
                        f"--user sentinel-client --pass {self.credentials.sentinel_client} "
                        "--no-auth-warning --raw SENTINEL replicas "
                        f"{MASTER_NAME}",
                        check=False,
                    ).splitlines()
                    if any(value == str(replica_port) or value.endswith(f":{replica_port}") for value in output):
                        known_by.append(port)
                if attached and known_by == list(SENTINEL_PORTS):
                    return
                last = f"replica ROLE={role[:4]}, known by Sentinel ports={known_by}"
            except Exception as error:  # qualification polling retains the last cause
                last = repr(error)
            time.sleep(0.1)
        raise QualificationError(f"replica topology did not converge before total outage: {last}")

    def _control_container(self) -> str:
        for name in (*self.sentinels, *self.redis):
            if self.is_running(name) and name not in self.paused:
                return name
        raise QualificationError("no running control container")

    def _assert_ports_free(self, ports: Iterable[int]) -> None:
        occupied = [port for port in ports if port_open(self.remote.host, port)]
        if occupied:
            raise QualificationError(f"required test ports are already occupied: {occupied}")

    def _wait_ports(self, ports: Iterable[int], timeout: float = 15) -> None:
        pending = set(ports)
        deadline = time.monotonic() + timeout
        while pending and time.monotonic() < deadline:
            pending = {port for port in pending if not port_open(self.remote.host, port)}
            if pending:
                time.sleep(0.1)
        if pending:
            raise QualificationError(f"ports did not become ready: {sorted(pending)}")

    def _wait_replicas(self, expected: int) -> None:
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            info = self.redis_cli(REDIS_PORTS[0], "INFO", "replication")
            if f"connected_slaves:{expected}" in info:
                return
            time.sleep(0.1)
        raise QualificationError("replicas did not attach to the initial primary")

    def _write_redis_configuration(self) -> None:
        credentials = self.credentials
        acl = (
            "user default off\n"
            f"user admin on >{credentials.admin} ~* &* +@all\n"
            f"user verdandi on >{credentials.app} resetkeys ~verdandi:* resetchannels &verdandi:* +@all "
            "-flushall -flushdb -config -acl -shutdown -replicaof -slaveof -debug -module\n"
            f"user sentinel-monitor on >{credentials.monitor} resetkeys resetchannels &__sentinel__:hello +multi +slaveof +ping +exec +subscribe "
            "+config|rewrite +role +publish +info +client|setname +client|kill +script|kill\n"
            f"user replica-user on >{credentials.replica} resetkeys resetchannels +psync +replconf +ping\n"
        )
        for index, port in enumerate(REDIS_PORTS, 1):
            replica = "" if index == 1 else f"replicaof {self.remote.host} {REDIS_PORTS[0]}\n"
            config = f"""port {port}
bind 0.0.0.0
protected-mode no
daemonize no
save ""
appendonly no
logfile ""
dir /data
aclfile /test/users.acl
masteruser replica-user
masterauth {credentials.replica}
replica-announce-ip {self.remote.host}
replica-announce-port {port}
{replica}"""
            directory = f"{self.root}/redis-{index}"
            self.remote.write(f"{directory}/users.acl", acl)
            self.remote.write(f"{directory}/redis.conf", config)

    def _write_sentinel_configuration(self) -> None:
        credentials = self.credentials
        acl = (
            "user default off\n"
            f"user sentinel-admin on >{credentials.sentinel_admin} ~* &* +@all\n"
            f"user sentinel-client on >{credentials.sentinel_client} resetkeys resetchannels +auth +client|getname +client|id +client|setname "
            "+command +hello +ping +role +sentinel|get-master-addr-by-name +sentinel|master +sentinel|myid "
            "+sentinel|replicas +sentinel|sentinels +sentinel|masters\n"
        )
        for index, port in enumerate(SENTINEL_PORTS, 1):
            config = f"""port {port}
bind 0.0.0.0
protected-mode no
daemonize no
logfile ""
dir /tmp
aclfile /test/users.acl
sentinel monitor {MASTER_NAME} {self.remote.host} {REDIS_PORTS[0]} 2
sentinel down-after-milliseconds {MASTER_NAME} 1000
sentinel failover-timeout {MASTER_NAME} 5000
sentinel parallel-syncs {MASTER_NAME} 1
sentinel auth-user {MASTER_NAME} sentinel-monitor
sentinel auth-pass {MASTER_NAME} {credentials.monitor}
sentinel sentinel-user sentinel-admin
sentinel sentinel-pass {credentials.sentinel_admin}
sentinel announce-ip {self.remote.host}
sentinel announce-port {port}
"""
            directory = f"{self.root}/sentinel-{index}"
            self.remote.write(f"{directory}/users.acl", acl)
            self.remote.write(f"{directory}/sentinel.conf", config)


class Peer:
    def __init__(self, name: str, command: list[str], cwd: Path, environment: dict[str, str]):
        self.name = name
        self._stdout: queue.Queue[str | None] = queue.Queue()
        self._stderr: list[str] = []
        self._process = subprocess.Popen(
            command,
            cwd=cwd,
            env=environment,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
        )
        threading.Thread(target=self._read_stdout, daemon=True).start()
        threading.Thread(target=self._read_stderr, daemon=True).start()

    def close(self) -> None:
        if self._process.poll() is None:
            self._process.terminate()
            try:
                self._process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self._process.kill()
                self._process.wait(timeout=5)

    def send(self, command: str, expected: str, timeout: float = 40) -> str:
        if self._process.stdin is None:
            raise QualificationError(f"{self.name} stdin is unavailable")
        self._process.stdin.write(command + "\n")
        self._process.stdin.flush()
        return self.read(expected, timeout)

    def read(self, expected: str, timeout: float = 120) -> str:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                line = self._stdout.get(timeout=max(0.001, min(0.2, deadline - time.monotonic())))
            except queue.Empty:
                if self._process.poll() is not None:
                    break
                continue
            if line is None:
                break
            print(f"[{self.name}] {line}", flush=True)
            if line.startswith(expected):
                return line
        diagnostics = "\n".join(self._stderr[-20:])
        raise QualificationError(f"{self.name} did not emit {expected!r}; exit={self._process.poll()}\n{diagnostics}")

    def _read_stdout(self) -> None:
        stream = self._process.stdout
        if stream is None:
            self._stdout.put(None)
            return
        for line in stream:
            self._stdout.put(line.rstrip("\r\n"))
        self._stdout.put(None)

    def _read_stderr(self) -> None:
        stream = self._process.stderr
        if stream is None:
            return
        for line in stream:
            value = line.rstrip("\r\n")
            self._stderr.append(value)
            print(f"[{self.name}:diagnostic] {value}", file=sys.stderr, flush=True)


def port_open(host: str, port: int) -> bool:
    try:
        with socket.create_connection((host, port), timeout=0.2):
            return True
    except OSError:
        return False


def parse_ready(line: str) -> str:
    parts = line.split()
    if len(parts) != 2 or not re.fullmatch(r"[0-9a-f]{32}", parts[1]):
        raise QualificationError(f"invalid peer READY line: {line}")
    return parts[1]


def parse_generation(line: str) -> int:
    parts = line.split()
    if len(parts) < 4 or not parts[-1].isdigit():
        raise QualificationError(f"invalid CHECKED line: {line}")
    return int(parts[-1])


def registration_key(zone: str, uuid: str) -> str:
    return f"verdandi:registration:{zone}:{TYPE_NAME}:{uuid}"


def build_peers(repository: Path, output: Path) -> tuple[list[str], list[str]]:
    go_executable = output / ("go-peer.exe" if os.name == "nt" else "go-peer")
    subprocess.run(
        ["go", "build", "-o", str(go_executable), "."],
        cwd=repository / "testkit" / "sentinel" / "go-peer",
        check=True,
    )
    rust_directory = repository / "testkit" / "sentinel" / "rust-peer"
    subprocess.run(["cargo", "build", "--quiet"], cwd=rust_directory, check=True)
    rust_executable = rust_directory / "target" / "debug" / ("verdandi-sentinel-rust-peer.exe" if os.name == "nt" else "verdandi-sentinel-rust-peer")
    return [str(go_executable)], [str(rust_executable)]


def run_sdk_sentinel_tests(repository: Path, environment: dict[str, str]) -> None:
    subprocess.run(
        [
            "go",
            "test",
            "-tags=integration",
            "-run=^TestSentinelRegistrationAndSelectorIntegration$",
            "-count=1",
            "./...",
        ],
        cwd=repository / "sdk" / "go",
        env=environment,
        check=True,
    )
    subprocess.run(
        [
            "cargo",
            "test",
            "--test",
            "integration",
            "sentinel_registration_and_selector_reconcile",
            "--",
            "--ignored",
            "--nocapture",
        ],
        cwd=repository / "sdk" / "rust",
        env=environment,
        check=True,
    )


def qualify(repository: Path, topology: Topology, zone: str) -> dict[str, object]:
    credentials = topology.credentials
    sentinel_addresses = ",".join(f"{topology.remote.host}:{port}" for port in SENTINEL_PORTS)
    sentinel_url = (
        f"redis-sentinel://verdandi:{quote(credentials.app)}@"
        f"{topology.remote.host}:{SENTINEL_PORTS[0]}/0?"
        f"sentinelServiceName={MASTER_NAME}"
        f"&sentinelUsername=sentinel-client"
        f"&sentinelPassword={quote(credentials.sentinel_client)}"
        f"&node={topology.remote.host}:{SENTINEL_PORTS[1]}"
        f"&node={topology.remote.host}:{SENTINEL_PORTS[2]}"
    )
    environment = os.environ.copy()
    environment.update(
        {
            "VERDANDI_SENTINEL_ADDRS": sentinel_addresses,
            "VERDANDI_SENTINEL_MASTER": MASTER_NAME,
            "VERDANDI_REDIS_USERNAME": "verdandi",
            "VERDANDI_REDIS_PASSWORD": credentials.app,
            "VERDANDI_SENTINEL_USERNAME": "sentinel-client",
            "VERDANDI_SENTINEL_PASSWORD": credentials.sentinel_client,
            "VERDANDI_SENTINEL_URL": sentinel_url,
        }
    )

    started = time.monotonic()
    run_sdk_sentinel_tests(repository, environment)
    with tempfile.TemporaryDirectory(prefix="verdandi-sentinel-", ignore_cleanup_errors=True) as temporary:
        go_command, rust_command = build_peers(repository, Path(temporary))
        go = Peer("go", [*go_command, zone], repository, environment)
        rust = Peer("rust", [*rust_command, zone], repository, environment)
        try:
            go_uuid = parse_ready(go.read("READY"))
            rust_uuid = parse_ready(rust.read("READY"))
            initial_go_generation = parse_generation(go.send(f"CHECK {rust_uuid} initial", "CHECKED"))
            initial_rust_generation = parse_generation(rust.send(f"CHECK {go_uuid} initial", "CHECKED"))

            go.send("UPDATE before-go", "UPDATED")
            rust.send("UPDATE before-rust", "UPDATED")
            go.send(f"CHECK {rust_uuid} before-rust", "CHECKED")
            rust.send(f"CHECK {go_uuid} before-go", "CHECKED")

            old_master = topology.master_port()
            topology.stop_sentinel(2)
            stale_monitor = topology.sentinel_monitor_line(2)
            if f" {old_master} " not in f" {stale_monitor} ":
                raise QualificationError("stopped Sentinel did not retain the old primary")

            replica_indices = [index for index, port in enumerate(REDIS_PORTS) if port != old_master]
            for index in replica_indices:
                topology.pause_redis(index)
            topology.redis_cli(old_master, "CLIENT", "KILL", "TYPE", "REPLICA")
            go_update = go.send("UPDATE lost-go", "UPDATED")
            rust_update = rust.send("UPDATE lost-rust", "UPDATED")
            if not go_update.endswith(" ok") or not rust_update.endswith(" ok"):
                raise QualificationError("acknowledged-loss setup did not receive definite writes")
            go_revision = int(go_update.split()[2])
            rust_revision = int(rust_update.split()[2])
            topology.kill_redis_port(old_master)
            for index in replica_indices:
                topology.unpause_redis(index)

            new_master = topology.wait_master(old_master, timeout=25)
            stale_go = topology.redis_cli(new_master, "HGET", registration_key(zone, go_uuid), "@revision").strip()
            stale_rust = topology.redis_cli(new_master, "HGET", registration_key(zone, rust_uuid), "@revision").strip()
            if not stale_go.isdigit() or int(stale_go) >= go_revision:
                raise QualificationError("Go acknowledged write was not demonstrably lost")
            if not stale_rust.isdigit() or int(stale_rust) >= rust_revision:
                raise QualificationError("Rust acknowledged write was not demonstrably lost")

            go.send("RENEW", "RENEWED")
            rust.send("RENEW", "RENEWED")
            go_generation = parse_generation(go.send(f"CHECK {rust_uuid} lost-rust", "CHECKED"))
            rust_generation = parse_generation(rust.send(f"CHECK {go_uuid} lost-go", "CHECKED"))
            if go_generation <= initial_go_generation or rust_generation <= initial_rust_generation:
                raise QualificationError("Selector generation did not advance after failover")

            topology.start_sentinel(2)
            topology.wait_sentinel_agreement(new_master)
            surviving_replica = next(port for port in REDIS_PORTS if port not in {old_master, new_master})
            topology.wait_replica_ready(new_master, surviving_replica)

            topology.redis_cli(new_master, "SCRIPT", "FLUSH")
            go.send("UPDATE noscript-go", "UPDATED")
            rust.send("UPDATE noscript-rust", "UPDATED")
            go.send(f"CHECK {rust_uuid} noscript-rust", "CHECKED")
            rust.send(f"CHECK {go_uuid} noscript-go", "CHECKED")

            for index in range(3):
                topology.stop_sentinel(index)
            go.send("UPDATE no-sentinel-go", "UPDATED")
            rust.send("UPDATE no-sentinel-rust", "UPDATED")
            go.send(f"CHECK {rust_uuid} no-sentinel-rust", "CHECKED")
            rust.send(f"CHECK {go_uuid} no-sentinel-go", "CHECKED")
            topology.kill_redis_port(new_master)
            go.send("WAIT_UNSYNC", "UNSYNCHRONIZED", timeout=20)
            rust.send("WAIT_UNSYNC", "UNSYNCHRONIZED", timeout=20)
            time.sleep(2)

            for index in range(3):
                topology.start_sentinel(index)
            # Restarted Sentinels first reload their persisted view of the dead
            # primary before quorum can complete a second promotion. Keep this
            # bounded, but allow slow hosts more than the ordinary first-failover
            # window.
            final_master = topology.wait_master(new_master, timeout=60)
            final_go_generation = parse_generation(go.send(f"CHECK {rust_uuid} no-sentinel-rust", "CHECKED", timeout=45))
            final_rust_generation = parse_generation(rust.send(f"CHECK {go_uuid} no-sentinel-go", "CHECKED", timeout=45))
            if final_go_generation <= go_generation or final_rust_generation <= rust_generation:
                raise QualificationError("Selector generation did not advance after total Sentinel loss")

            go.send("UPDATE recovered-go", "UPDATED")
            rust.send("UPDATE recovered-rust", "UPDATED")
            go.send(f"CHECK {rust_uuid} recovered-rust", "CHECKED")
            rust.send(f"CHECK {go_uuid} recovered-go", "CHECKED")
            go.send("STOP", "STOPPED")
            rust.send("STOP", "STOPPED")
            topology.redis_cli(final_master, "DEL", f"verdandi:config:{zone}")
            return {
                "status": "pass",
                "redis_version": "8.8.0",
                "initial_master": old_master,
                "acknowledged_loss_master": new_master,
                "recovered_master": final_master,
                "go_uuid_preserved": go_uuid,
                "rust_uuid_preserved": rust_uuid,
                "go_selector_generations": [
                    initial_go_generation,
                    go_generation,
                    final_go_generation,
                ],
                "rust_selector_generations": [
                    initial_rust_generation,
                    rust_generation,
                    final_rust_generation,
                ],
                "scenarios": [
                    "Go and Rust SDK Sentinel integration suites",
                    "three Sentinels with separate ACL credentials",
                    "minority Sentinel unavailable and stale configuration",
                    "forced acknowledged write loss and full-state republish",
                    "Pub/Sub generation recovery",
                    "SCRIPT FLUSH reload",
                    "all Sentinels unavailable",
                    "primary loss while all Sentinels are unavailable",
                    "Sentinel restart and second promotion",
                    "Go/Rust cross-language convergence",
                ],
                "elapsed_seconds": round(time.monotonic() - started, 3),
            }
        finally:
            go.close()
            rust.close()


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="192.168.0.90")
    parser.add_argument("--ssh-user", default="ubuntu")
    parser.add_argument(
        "--ssh-password-env",
        default="VERDANDI_TEST_SSH_PASSWORD",
        help="environment variable containing the SSH password",
    )
    parser.add_argument(
        "--keep-topology",
        action="store_true",
        help="retain only this harness run's containers and /tmp directory",
    )
    parser.add_argument(
        "--result-file",
        help="optional path receiving the complete JSON result",
    )
    return parser.parse_args()


def main() -> int:
    options = arguments()
    password = os.environ.get(options.ssh_password_env)
    if not password:
        print(f"missing {options.ssh_password_env}", file=sys.stderr)
        return 2
    repository = Path(__file__).resolve().parents[2]
    run_id = secrets.token_hex(4)
    zone = "Sentinel" + "".join(chr(ord("a") + value % 26) for value in os.urandom(8))
    remote = Remote(options.host, options.ssh_user, password)
    topology = Topology(remote, run_id, Credentials.generate())
    try:
        topology.deploy()
        result = qualify(repository, topology, zone)
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
