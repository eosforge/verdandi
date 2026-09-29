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

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from testkit.support import Peer, build_peers
from contextlib import ExitStack

REPOSITORY = Path(__file__).resolve().parents[2]


def start_peers(zone, go_environment, rust_environment):
    go, rust = build_peers("catalog")
    with ExitStack() as owners:
        go_peer = owners.enter_context(Peer([go, zone], REPOSITORY, go_environment))
        rust_peer = owners.enter_context(Peer([rust, zone], REPOSITORY, rust_environment))
        go_peer.wait_line("READY")
        rust_peer.wait_line("READY")
        owners.pop_all()
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
    client = redis.Redis.from_url(arguments.redis_url, socket_connect_timeout=5, socket_timeout=5)
    go_peer: Peer | None = None
    rust_peer: Peer | None = None
    started = time.monotonic()
    result: dict[str, Any] = {"status": "starting", "zone": zone}
    owned_zone = False
    try:
        if catalog_keys(client, zone):
            raise RuntimeError("random Catalog interoperability Zone already exists")
        owned_zone = True
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
        with ExitStack() as cleanup:
            cleanup.callback(client.close)
            if owned_zone:

                def remove_keys():
                    if keys := catalog_keys(client, zone):
                        client.unlink(*keys)

                cleanup.callback(remove_keys)
            for peer in (go_peer, rust_peer):
                if peer is not None:
                    cleanup.callback(peer.stop)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"FAIL {error}", file=sys.stderr)
        raise
