#!/usr/bin/env python3
"""Run live Go-to-Rust and Rust-to-Go Registration/Catalog interoperability."""

from __future__ import annotations

import argparse
import os
import queue
import random
import subprocess
import sys
import threading
import time
from pathlib import Path

import redis

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from testkit.support import Peer, build_peers
from contextlib import ExitStack


def zone_name() -> str:
    return "Interop" + "".join(random.choice("abcdefghijklmnopqrstuvwxyz") for _ in range(12))


def zone_keys(client: redis.Redis, zone: str) -> list[bytes]:
    keys = list(client.scan_iter(match=f"verdandi:*:{zone}:*", count=256))
    config = f"verdandi:config:{zone}".encode()
    if client.exists(config):
        keys.append(config)
    return sorted(set(keys))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--redis-url", required=True)
    arguments = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    zone = zone_name()
    client = redis.Redis.from_url(arguments.redis_url, socket_connect_timeout=5, socket_timeout=5)
    go_peer: Peer | None = None
    rust_peer: Peer | None = None
    owned_zone = False
    try:
        if zone_keys(client, zone):
            raise RuntimeError("random interoperability Zone already exists")
        owned_zone = True
        go, rust = build_peers("interop")
        go_peer = Peer([go, arguments.redis_url, zone], root)
        rust_peer = Peer([rust, arguments.redis_url, zone], root)
        go_peer.wait_line("READY")
        rust_peer.wait_line("READY")

        go_peer.send("PRODUCE")
        go_uuid = go_peer.wait_line("GO ").split()[1]
        rust_peer.send(f"VERIFY_GO {go_uuid}")
        rust_peer.wait_line("VERIFIED_GO")

        rust_peer.send("PRODUCE")
        rust_uuid = rust_peer.wait_line("RUST ").split()[1]
        go_peer.send(f"VERIFY_RUST {rust_uuid}")
        go_peer.wait_line("VERIFIED_RUST")

        go_peer.send("CATALOG_PRODUCE")
        if go_peer.wait_line("CATALOG_GO ").split()[1] != "1":
            raise RuntimeError("Go Catalog publication did not create revision 1")
        rust_peer.send("VERIFY_CATALOG_GO")
        rust_peer.wait_line("VERIFIED_CATALOG_GO")

        rust_peer.send("CATALOG_PRODUCE")
        if rust_peer.wait_line("CATALOG_RUST ").split()[1] != "2":
            raise RuntimeError("Rust Catalog publication did not create revision 2")
        go_peer.send("VERIFY_CATALOG_RUST")
        go_peer.wait_line("VERIFIED_CATALOG_RUST")

        go_peer.send("CATALOG_DELETE")
        if go_peer.wait_line("CATALOG_DELETED ").split()[1] != "3":
            raise RuntimeError("Go Catalog Delete did not create revision 3")
        rust_peer.send("VERIFY_CATALOG_DELETE")
        rust_peer.wait_line("VERIFIED_CATALOG_DELETE")

        go_peer.send("STOP")
        rust_peer.send("STOP")
        go_peer.wait_line("STOPPED")
        rust_peer.wait_line("STOPPED")
        if go_peer.process.wait(timeout=10) != 0 or rust_peer.process.wait(timeout=10) != 0:
            raise RuntimeError("peer returned a non-zero status")
        catalog_keys = list(client.scan_iter(match=f"verdandi:catalog:{zone}:*", count=256))
        if catalog_keys:
            client.delete(*catalog_keys)
        remaining = zone_keys(client, zone)
        config_key = f"verdandi:config:{zone}".encode()
        if remaining != [config_key]:
            raise RuntimeError(f"graceful cleanup should retain only Zone config, got: {remaining!r}")
        client.delete(config_key)
        if zone_keys(client, zone):
            raise RuntimeError("test-owned Zone cleanup did not converge")
        print(f"PASS Go->Rust and Rust->Go live Registration/Catalog interoperability on Zone {zone}")
        return 0
    finally:
        with ExitStack() as cleanup:
            cleanup.callback(client.close)
            if owned_zone:

                def remove_keys():
                    if keys := zone_keys(client, zone):
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
