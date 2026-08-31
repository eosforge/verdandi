#!/usr/bin/env python3
"""Run Catalog-only Go-to-Rust and Rust-to-Go live interoperability."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import queue
import random
import subprocess
import sys
import threading
import time
from typing import Any

import redis

REPOSITORY = Path(__file__).resolve().parents[2]


class Peer:
    def __init__(self, command: list[str], cwd: Path, environment: dict[str, str]) -> None:
        self.stderr: list[str] = []
        self.lines: queue.Queue[str] = queue.Queue()
        self.process = subprocess.Popen(
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

    def _read_stdout(self) -> None:
        if self.process.stdout is None:
            return
        for line in self.process.stdout:
            self.lines.put(line.rstrip("\r\n"))

    def _read_stderr(self) -> None:
        if self.process.stderr is None:
            return
        self.stderr.extend(line.rstrip("\r\n") for line in self.process.stderr)

    def wait_line(self, prefix: str, timeout: float = 90.0) -> str:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.process.poll() is not None and self.lines.empty():
                raise RuntimeError(f"peer exited with {self.process.returncode}: {' | '.join(self.stderr)}")
            try:
                line = self.lines.get(timeout=min(0.2, deadline - time.monotonic()))
            except queue.Empty:
                continue
            if line.startswith("ERROR "):
                raise RuntimeError(line)
            if line.startswith(prefix):
                return line
            raise RuntimeError(f"unexpected peer output: {line!r}")
        raise TimeoutError(f"timed out waiting for {prefix!r}: {' | '.join(self.stderr)}")

    def send(self, command: str) -> None:
        if self.process.stdin is None:
            raise RuntimeError("peer stdin is closed")
        self.process.stdin.write(command + "\n")
        self.process.stdin.flush()

    def command(self, command: str, prefix: str, timeout: float = 90.0) -> str:
        self.send(command)
        return self.wait_line(prefix, timeout)

    def stop(self) -> None:
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)


def start_peers(
    zone: str,
    go_environment: dict[str, str],
    rust_environment: dict[str, str],
) -> tuple[Peer, Peer]:
    go_peer = Peer(
        ["go", "run", ".", zone],
        REPOSITORY / "testkit" / "catalog" / "go-peer",
        go_environment,
    )
    try:
        rust_peer = Peer(
            [
                "cargo",
                "run",
                "--quiet",
                "--manifest-path",
                str(REPOSITORY / "testkit" / "catalog" / "rust-peer" / "Cargo.toml"),
                "--",
                zone,
            ],
            REPOSITORY,
            rust_environment,
        )
    except BaseException:
        go_peer.stop()
        raise
    go_peer.wait_line("READY")
    rust_peer.wait_line("READY")
    return go_peer, rust_peer


def revision(peer: Peer, command: str) -> int:
    return int(peer.command(command, "REVISION ").split()[1])


def check(peer: Peer, command: str) -> None:
    peer.command(command, "CHECKED")


def run_sequence(go_peer: Peer, rust_peer: Peer) -> list[dict[str, Any]]:
    operations: list[dict[str, Any]] = []
    value = revision(go_peer, "REPLACE go 1")
    if value != 1:
        raise RuntimeError(f"Go Replace returned revision {value}, want 1")
    check(rust_peer, "CHECK 1 go 1")
    operations.append({"revision": 1, "publisher": "go", "operation": "replace"})

    value = revision(rust_peer, "PATCH 1 rust 2")
    if value != 2:
        raise RuntimeError(f"Rust Patch returned revision {value}, want 2")
    check(go_peer, "CHECK 2 rust 2")
    operations.append({"revision": 2, "publisher": "rust", "operation": "patch"})

    value = revision(go_peer, "DELETE")
    if value != 3:
        raise RuntimeError(f"Go Delete returned revision {value}, want 3")
    check(rust_peer, "CHECK_DELETED 3")
    operations.append({"revision": 3, "publisher": "go", "operation": "delete"})

    value = revision(rust_peer, "REPLACE rust 4")
    if value != 4:
        raise RuntimeError(f"Rust Replace returned revision {value}, want 4")
    check(go_peer, "CHECK 4 rust 4")
    operations.append({"revision": 4, "publisher": "rust", "operation": "replace"})

    value = revision(go_peer, "PATCH 4 go 5")
    if value != 5:
        raise RuntimeError(f"Go Patch returned revision {value}, want 5")
    check(rust_peer, "CHECK 5 go 5")
    operations.append({"revision": 5, "publisher": "go", "operation": "patch"})

    value = revision(rust_peer, "DELETE")
    if value != 6:
        raise RuntimeError(f"Rust Delete returned revision {value}, want 6")
    check(go_peer, "CHECK_DELETED 6")
    operations.append({"revision": 6, "publisher": "rust", "operation": "delete"})
    return operations


def stop_peers(go_peer: Peer, rust_peer: Peer) -> None:
    go_peer.send("STOP")
    rust_peer.send("STOP")
    go_peer.wait_line("STOPPED")
    rust_peer.wait_line("STOPPED")
    if go_peer.process.wait(timeout=10) != 0 or rust_peer.process.wait(timeout=10) != 0:
        raise RuntimeError("Catalog peer returned a non-zero status")


def zone_name() -> str:
    return "CatalogInterop" + "".join(random.choice("abcdefghijklmnopqrstuvwxyz") for _ in range(12))


def catalog_keys(client: redis.Redis, zone: str) -> list[bytes]:
    return list(client.scan_iter(match=f"verdandi:catalog:{zone}:*", count=256))


def write_result(path: str | None, value: dict[str, Any]) -> None:
    if path is None:
        return
    target = Path(path).resolve()
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--redis-url", required=True)
    parser.add_argument("--result-file")
    arguments = parser.parse_args()
    zone = zone_name()
    client = redis.Redis.from_url(arguments.redis_url)
    go_peer: Peer | None = None
    rust_peer: Peer | None = None
    started = time.monotonic()
    result: dict[str, Any] = {"status": "starting", "zone": zone}
    try:
        if catalog_keys(client, zone):
            raise RuntimeError("random Catalog interoperability Zone already exists")
        go_environment = os.environ.copy()
        go_environment["VERDANDI_REDIS_URL"] = arguments.redis_url
        rust_environment = os.environ.copy()
        rust_environment["VERDANDI_CATALOG_ENDPOINT"] = arguments.redis_url
        go_peer, rust_peer = start_peers(zone, go_environment, rust_environment)
        operations = run_sequence(go_peer, rust_peer)
        stop_peers(go_peer, rust_peer)
        keys = catalog_keys(client, zone)
        if keys:
            client.unlink(*keys)
        if catalog_keys(client, zone):
            raise RuntimeError("Catalog interoperability cleanup did not converge")
        result.update(
            {
                "status": "pass",
                "elapsed_seconds": round(time.monotonic() - started, 3),
                "operations": operations,
                "final_keys": 0,
            }
        )
        write_result(arguments.result_file, result)
        print(f"PASS Catalog-only Go<->Rust interoperability on Zone {zone}: " f"{len(operations)} cross-language revisions")
        return 0
    except BaseException as error:
        result.update(
            {
                "status": "failed",
                "elapsed_seconds": round(time.monotonic() - started, 3),
                "failure": str(error),
            }
        )
        write_result(arguments.result_file, result)
        raise
    finally:
        if go_peer is not None:
            go_peer.stop()
        if rust_peer is not None:
            rust_peer.stop()
        keys = catalog_keys(client, zone)
        if keys:
            client.unlink(*keys)
        client.close()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"FAIL {error}", file=sys.stderr)
        raise
