#!/usr/bin/env python3
"""Qualify Catalog-only Go/Rust clients across two Redis 8 Sentinel promotions."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import secrets
import sys
import time
from typing import Any
from urllib.parse import quote

REPOSITORY = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPOSITORY))

from testkit.catalog.interop_test import (  # noqa: E402
    Peer,
    check,
    revision,
    start_peers,
    stop_peers,
)
from testkit.sentinel.sentinel_test import (  # noqa: E402
    MASTER_NAME,
    REDIS_PORTS,
    SENTINEL_PORTS,
    Credentials,
    QualificationError,
    Remote,
    Topology,
)


def expect_revision(peer: Peer, command: str, expected: int) -> None:
    actual = revision(peer, command)
    if actual != expected:
        raise QualificationError(f"{command} returned Catalog revision {actual}, want {expected}")


def wait_replicas(topology: Topology, count: int) -> None:
    value = topology.redis_cli(topology.master_port(), "WAIT", str(count), "5000").strip()
    if value != str(count):
        raise QualificationError(f"WAIT returned {value!r}, want {count}")


def cleanup_zone(topology: Topology, zone: str) -> int:
    master = topology.master_port()
    cursor = "0"
    removed = 0
    while True:
        response = topology.redis_cli(
            master,
            "SCAN",
            cursor,
            "MATCH",
            f"verdandi:catalog:{zone}:*",
            "COUNT",
            "256",
        ).splitlines()
        if not response:
            raise QualificationError("Catalog cleanup SCAN returned no cursor")
        cursor = response[0]
        keys = [key for key in response[1:] if key]
        if keys:
            removed += int(topology.redis_cli(master, "UNLINK", *keys).strip())
        if cursor == "0":
            break
    remaining = topology.redis_cli(
        master,
        "SCAN",
        "0",
        "MATCH",
        f"verdandi:catalog:{zone}:*",
        "COUNT",
        "256",
    ).splitlines()
    remaining_keys = [key for key in remaining[1:] if key]
    if not remaining or remaining[0] != "0" or remaining_keys:
        raise QualificationError(f"Catalog Sentinel cleanup left keys: {remaining_keys}")
    return removed


def qualify(topology: Topology, zone: str) -> dict[str, Any]:
    credentials = topology.credentials
    addresses = ",".join(f"{topology.remote.host}:{port}" for port in SENTINEL_PORTS)
    sentinel_url = (
        f"redis-sentinel://verdandi:{quote(credentials.app)}@"
        f"{topology.remote.host}:{SENTINEL_PORTS[0]}/0?"
        f"sentinelServiceName={MASTER_NAME}"
        f"&sentinelUsername=sentinel-client"
        f"&sentinelPassword={quote(credentials.sentinel_client)}"
        f"&node={topology.remote.host}:{SENTINEL_PORTS[1]}"
        f"&node={topology.remote.host}:{SENTINEL_PORTS[2]}"
    )
    go_environment = os.environ.copy()
    go_environment.update(
        {
            "VERDANDI_CATALOG_SENTINELS": addresses,
            "VERDANDI_CATALOG_SENTINEL_MASTER": MASTER_NAME,
            "VERDANDI_CATALOG_USERNAME": "verdandi",
            "VERDANDI_CATALOG_PASSWORD": credentials.app,
            "VERDANDI_CATALOG_SENTINEL_USERNAME": "sentinel-client",
            "VERDANDI_CATALOG_SENTINEL_PASSWORD": credentials.sentinel_client,
        }
    )
    rust_environment = os.environ.copy()
    rust_environment["VERDANDI_CATALOG_ENDPOINT"] = sentinel_url
    started = time.monotonic()
    events: list[dict[str, Any]] = []
    go_peer: Peer | None = None
    rust_peer: Peer | None = None
    try:
        go_peer, rust_peer = start_peers(zone, go_environment, rust_environment)

        expect_revision(go_peer, "REPLACE go 1", 1)
        check(rust_peer, "CHECK 1 go 1")
        events.append({"revision": 1, "operation": "go_replace"})

        initial_master = topology.master_port()
        topology.redis_cli(initial_master, "SCRIPT", "FLUSH")
        expect_revision(rust_peer, "PATCH 1 rust 2", 2)
        check(go_peer, "CHECK 2 rust 2")
        events.append({"revision": 2, "operation": "rust_patch_after_script_flush"})

        topology.redis_cli(initial_master, "SCRIPT", "FLUSH")
        expect_revision(go_peer, "PATCH 2 go 3", 3)
        check(rust_peer, "CHECK 3 go 3")
        wait_replicas(topology, 2)
        events.append({"revision": 3, "operation": "go_patch_after_script_flush"})

        topology.kill_redis_port(initial_master)
        first_promotion = topology.wait_master(different_from=initial_master, timeout=30)
        topology.wait_sentinel_agreement(first_promotion, timeout=30)
        remaining_replica = next(port for port in REDIS_PORTS if port not in {initial_master, first_promotion})
        topology.wait_replica_ready(first_promotion, remaining_replica, timeout=30)
        expect_revision(rust_peer, "REPLACE rust 4", 4)
        check(go_peer, "CHECK 4 rust 4")
        expect_revision(go_peer, "PATCH 4 go 5", 5)
        check(rust_peer, "CHECK 5 go 5")
        wait_replicas(topology, 1)
        events.append(
            {
                "revision": 5,
                "operation": "first_promotion_recovery",
                "old_master": initial_master,
                "new_master": first_promotion,
            }
        )

        topology.kill_redis_port(first_promotion)
        second_promotion = topology.wait_master(different_from=first_promotion, timeout=30)
        topology.wait_sentinel_agreement(second_promotion, timeout=30)
        if second_promotion in {initial_master, first_promotion}:
            raise QualificationError("second promotion did not select the last live Redis")
        expect_revision(go_peer, "REPLACE go 6", 6)
        check(rust_peer, "CHECK 6 go 6")
        expect_revision(rust_peer, "PATCH 6 rust 7", 7)
        check(go_peer, "CHECK 7 rust 7")
        expect_revision(go_peer, "DELETE", 8)
        check(rust_peer, "CHECK_DELETED 8")
        expect_revision(rust_peer, "REPLACE rust 9", 9)
        check(go_peer, "CHECK 9 rust 9")
        expect_revision(rust_peer, "DELETE", 10)
        check(go_peer, "CHECK_DELETED 10")
        events.append(
            {
                "revision": 10,
                "operation": "second_promotion_recovery_and_delete",
                "old_master": first_promotion,
                "new_master": second_promotion,
            }
        )

        stop_peers(go_peer, rust_peer)
        removed = cleanup_zone(topology, zone)
        version = topology.redis_cli(second_promotion, "INFO", "server")
        if "redis_version:8.8.0" not in version:
            raise QualificationError("Catalog Sentinel topology was not Redis 8.8.0")
        return {
            "status": "pass",
            "zone": zone,
            "elapsed_seconds": round(time.monotonic() - started, 3),
            "redis_version": "8.8.0",
            "initial_master": initial_master,
            "first_promotion": first_promotion,
            "second_promotion": second_promotion,
            "events": events,
            "final_revision": 10,
            "cleanup_keys": removed,
            "final_keys": 0,
        }
    finally:
        if go_peer is not None:
            go_peer.stop()
        if rust_peer is not None:
            rust_peer.stop()


def options() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="192.168.0.90")
    parser.add_argument("--ssh-user", default="ubuntu")
    parser.add_argument("--ssh-password-env", default="VERDANDI_TEST_SSH_PASSWORD")
    parser.add_argument("--keep-topology", action="store_true")
    parser.add_argument("--result-file")
    return parser.parse_args()


def main() -> int:
    arguments = options()
    password = os.environ.get(arguments.ssh_password_env)
    if not password:
        print(f"missing {arguments.ssh_password_env}", file=sys.stderr)
        return 2
    run_id = secrets.token_hex(4)
    zone = "CatalogSentinel" + "".join(chr(ord("a") + value % 26) for value in os.urandom(8))
    remote = Remote(arguments.host, arguments.ssh_user, password)
    topology = Topology(remote, run_id, Credentials.generate())
    try:
        topology.deploy()
        result = qualify(topology, zone)
        serialized = json.dumps(result, indent=2, sort_keys=True)
        if arguments.result_file:
            target = Path(arguments.result_file).resolve()
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(serialized + "\n", encoding="utf-8")
        print(serialized)
        return 0
    except Exception as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    finally:
        if not arguments.keep_topology:
            topology.cleanup()
        remote.close()


if __name__ == "__main__":
    raise SystemExit(main())
