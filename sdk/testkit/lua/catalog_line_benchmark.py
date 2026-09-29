#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 LaconisIves

"""Measure the current Catalog Hash/ZSET Lua protocol on Redis 8."""

from __future__ import annotations

import argparse
from collections.abc import Callable
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import secrets
import statistics
import string
import time
from typing import Any

import redis

from catalog_test import CatalogScripts, cleanup_zone, encoded_size
import generate_catalog

ROOT = Path(__file__).resolve().parents[2]


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    index = min(len(ordered) - 1, int(len(ordered) * fraction))
    return ordered[index]


def command_counters(
    client: redis.Redis,
    names: tuple[str, ...],
) -> dict[str, tuple[int, int]]:
    info = client.info("commandstats")
    result: dict[str, tuple[int, int]] = {}
    for name in names:
        values = info.get(f"cmdstat_{name}", {})
        result[name] = (int(values.get("calls", 0)), int(values.get("usec", 0)))
    return result


def measure(
    client: redis.Redis,
    name: str,
    calls: int,
    operation: Callable[[int], None],
    commands: tuple[str, ...] = ("evalsha",),
) -> dict[str, Any]:
    before = command_counters(client, commands)
    latencies: list[float] = []
    started = time.perf_counter_ns()
    for index in range(calls):
        call_started = time.perf_counter_ns()
        operation(index)
        latencies.append((time.perf_counter_ns() - call_started) / 1_000)
    elapsed_seconds = (time.perf_counter_ns() - started) / 1_000_000_000
    after = command_counters(client, commands)
    command_delta: dict[str, Any] = {}
    for command in commands:
        before_calls, before_usec = before[command]
        after_calls, after_usec = after[command]
        delta_calls = after_calls - before_calls
        delta_usec = after_usec - before_usec
        command_delta[command] = {
            "calls": delta_calls,
            "server_usec": delta_usec,
            "server_usec_per_logical_call": (delta_usec / calls if calls else 0.0),
        }
    return {
        "scenario": name,
        "calls": calls,
        "throughput_per_second": calls / elapsed_seconds,
        "wall_usec": {
            "mean": statistics.fmean(latencies),
            "p50": percentile(latencies, 0.50),
            "p95": percentile(latencies, 0.95),
            "p99": percentile(latencies, 0.99),
            "maximum": max(latencies),
        },
        "commands": command_delta,
    }


def require_ok(reply: dict[bytes, Any]) -> int:
    if reply.get(b"&result") != b"ok":
        raise AssertionError(f"Catalog operation failed: {reply!r}")
    revision = reply.get(b"@revision")
    if not isinstance(revision, bytes):
        raise AssertionError(f"Catalog revision missing: {reply!r}")
    return int(revision)


def seed(
    scripts: CatalogScripts,
    fields: list[tuple[str, bytes]],
    kind: str = "map",
) -> int:
    return require_ok(scripts.replace(kind, fields))


def benchmark(
    client: redis.Redis,
    zone: str,
    iterations: int,
    wide_fields: int,
) -> list[dict[str, Any]]:
    results: list[dict[str, Any]] = []

    replace = CatalogScripts(client, zone, "bench", "replace-small")
    small_fields = [("route", b"east"), ("weight", b"100")]

    def replace_small_once(_: int) -> None:
        require_ok(replace.replace("map", small_fields))

    results.append(
        measure(
            client,
            "replace_small_map",
            iterations,
            replace_small_once,
        )
    )

    patch = CatalogScripts(client, zone, "bench", "patch-small")
    patch_revision = seed(patch, [("route", b"east"), ("weight", b"100")])

    def patch_once(index: int) -> None:
        nonlocal patch_revision
        header = client.hmget(
            patch.key,
            "@revision",
            "@kind",
            "@encoded_bytes",
            "route",
        )
        if header[0] != str(patch_revision).encode():
            raise AssertionError(f"projection header mismatch: {header!r}")
        value = b"west" if index % 2 == 0 else b"east"
        projected = len("route") + len(value) + len("weight") + len(b"100")
        patch_revision = require_ok(
            patch.patch(
                patch_revision,
                projected,
                [("route", value)],
            )
        )

    results.append(
        measure(
            client,
            "patch_one_map_field_with_projection",
            iterations,
            patch_once,
            ("evalsha", "hmget"),
        )
    )

    read_full = CatalogScripts(client, zone, "bench", "read-full")
    read_full_revision = seed(
        read_full,
        [(f"f{index:04d}", b"x" * 16) for index in range(64)],
    )

    def read_full_once(_: int) -> None:
        reply = read_full.read(0)
        if reply.get(b"&result") != b"ok" or reply.get(b"&mode") != b"replace" or reply.get(b"@revision") != str(read_full_revision).encode():
            raise AssertionError(f"full read failed: {reply!r}")

    results.append(measure(client, "read_full_64_fields", iterations, read_full_once))

    read_delta = CatalogScripts(client, zone, "bench", "read-delta")
    read_base = seed(
        read_delta,
        [(f"f{index:04d}", b"x" * 16) for index in range(64)],
    )
    delta_revision = require_ok(
        read_delta.patch(
            read_base,
            encoded_size([(f"f{index:04d}", b"y" * 16 if index == 31 else b"x" * 16) for index in range(64)]),
            [("f0031", b"y" * 16)],
        )
    )

    def read_delta_once(_: int) -> None:
        reply = read_delta.read(read_base)
        if reply.get(b"&result") != b"ok" or reply.get(b"&mode") != b"patch" or reply.get(b"@revision") != str(delta_revision).encode():
            raise AssertionError(f"delta read failed: {reply!r}")

    results.append(measure(client, "read_one_field_delta", iterations, read_delta_once))

    unchanged = CatalogScripts(client, zone, "bench", "read-unchanged")
    unchanged_revision = seed(unchanged, [("route", b"east")])

    def read_unchanged_once(_: int) -> None:
        reply = unchanged.read(unchanged_revision)
        if reply.get(b"&mode") != b"unchanged":
            raise AssertionError(f"unchanged read failed: {reply!r}")

    results.append(measure(client, "read_unchanged", iterations, read_unchanged_once))

    delete = CatalogScripts(client, zone, "bench", "delete")
    delete_revision = 0

    def delete_once(_: int) -> None:
        nonlocal delete_revision
        delete_revision = require_ok(delete.delete())

    results.append(measure(client, "delete_refresh_tombstone", iterations, delete_once))
    if delete_revision == 0:
        raise AssertionError("delete benchmark did not advance revision")

    wide = CatalogScripts(client, zone, "bench", "replace-wide")
    wide_values = [(f"f{index:05d}", bytes([index % 251]) * 32) for index in range(wide_fields)]
    wide_calls = max(50, iterations // 10)

    def replace_wide_once(_: int) -> None:
        require_ok(wide.replace("map", wide_values))

    results.append(
        measure(
            client,
            f"replace_{wide_fields}_field_map",
            wide_calls,
            replace_wide_once,
        )
    )
    return results


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--redis-url",
        default=os.environ.get("VERDANDI_REDIS_URL", "redis://127.0.0.1:6379/0"),
    )
    parser.add_argument("--iterations", type=int, default=2_000)
    parser.add_argument("--wide-fields", type=int, default=512)
    parser.add_argument("--output", type=Path)
    arguments = parser.parse_args()
    if arguments.iterations < 10 or not 1 <= arguments.wide_fields <= 8_192:
        raise ValueError("invalid benchmark bounds")

    generated = generate_catalog.expected_outputs(generate_catalog.load_manifest())
    if generate_catalog.check(generated) != 0:
        raise RuntimeError("Catalog Lua generated files are stale")

    client = redis.Redis.from_url(arguments.redis_url, decode_responses=False)
    server = client.info("server")
    version = tuple(int(part) for part in server["redis_version"].split(".")[:2])
    if version < (8, 0):
        raise RuntimeError(f"Redis 8.0 or later is required, found {version!r}")

    zone = "CatalogBench" + "".join(secrets.choice(string.ascii_letters) for _ in range(12))
    try:
        results = benchmark(
            client,
            zone,
            arguments.iterations,
            arguments.wide_fields,
        )
    finally:
        cleanup_zone(client, zone)

    document = {
        "schema": "verdandi.catalog.benchmark.v1",
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "redis_version": server["redis_version"],
        "iterations": arguments.iterations,
        "wide_fields": arguments.wide_fields,
        "results": results,
    }
    print("scenario                                  calls    ops/s" "   p50 us   p95 us   p99 us  server us/op")
    for result in results:
        evalsha = result["commands"]["evalsha"]
        print(
            f"{result['scenario']:<40}"
            f"{result['calls']:>7}"
            f"{result['throughput_per_second']:>9.1f}"
            f"{result['wall_usec']['p50']:>9.1f}"
            f"{result['wall_usec']['p95']:>9.1f}"
            f"{result['wall_usec']['p99']:>9.1f}"
            f"{evalsha['server_usec_per_logical_call']:>14.2f}"
        )
    if arguments.output is not None:
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.write_text(
            json.dumps(document, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )


if __name__ == "__main__":
    main()
