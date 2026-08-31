#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 LaconisIves

"""Measure generated Registration mutation hot paths on Redis 8."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import secrets
import statistics
import string
import time
from typing import Any, Iterable

import redis

ROOT = Path(__file__).resolve().parents[2]
GENERATED_ROOT = ROOT / "lua" / "registration"
KINDS = ("register", "update", "renew", "unregister")
TTL_MS = 120_000


def generated_sources() -> dict[str, str]:
    return {kind: (GENERATED_ROOT / f"{kind}.lua").read_text(encoding="utf-8") for kind in KINDS}


def decoded_pairs(reply: Iterable[Any]) -> dict[bytes, Any]:
    values = list(reply)
    if len(values) % 2 != 0:
        raise AssertionError(f"odd reply: {values!r}")
    decoded = dict(zip(values[::2], values[1::2], strict=True))
    if len(decoded) * 2 != len(values):
        raise AssertionError(f"duplicate reply field: {values!r}")
    return decoded


def assert_ok(replies: Iterable[Any]) -> None:
    for reply in replies:
        decoded = decoded_pairs(reply)
        if decoded.get(b"&result") != b"ok":
            raise AssertionError(f"operation failed: {decoded!r}")


class ScriptSet:
    def __init__(
        self,
        client: redis.Redis,
        overrides: dict[str, str] | None = None,
    ) -> None:
        sources = generated_sources()
        self.shas = {kind: client.script_load(source) for kind, source in sources.items()}
        if overrides:
            if client.script_exists(*overrides.values()) != [True] * len(overrides):
                raise RuntimeError("one or more cached baseline SHAs are unavailable")
            self.shas.update(overrides)

    def enqueue(
        self,
        pipeline: redis.client.Pipeline,
        kind: str,
        registration_key: str,
        registry_key: str,
        arguments: list[Any],
    ) -> None:
        pipeline.evalsha(
            self.shas[kind],
            2,
            registration_key,
            registry_key,
            *arguments,
        )


def register_arguments(uuid: str) -> list[Any]:
    arguments: list[Any] = [
        uuid,
        1,
        TTL_MS,
        1,
        ".build",
        b"benchmark",
        ".region",
        b"local",
    ]
    arguments.extend(("address", b"127.0.0.1:8080"))
    for index in range(30):
        arguments.extend((f"d{index:02d}", b"0"))
    arguments.extend(("load", b"0"))
    return arguments


def update_arguments(
    uuid: str,
    revision: int,
    value: bytes,
    version: int | None = None,
    *,
    wide: bool = False,
) -> list[Any]:
    arguments: list[Any] = [
        uuid,
        revision,
        "" if version is None else version,
    ]
    if wide:
        arguments.extend(("address", value))
        for index in range(30):
            arguments.extend((f"d{index:02d}", value))
    arguments.extend(("load", value))
    return arguments


def renew_arguments(uuid: str, revision: int) -> list[Any]:
    return [uuid, revision]


def unregister_arguments(uuid: str) -> list[Any]:
    return [uuid]


def execute_phase(
    client: redis.Redis,
    scripts: ScriptSet,
    kind: str,
    records: list[tuple[str, str]],
    registry_key: str,
    rounds: int,
    revision: int,
) -> dict[str, Any]:
    client.execute_command("CONFIG", "RESETSTAT")
    started = time.perf_counter()
    calls = 0
    for round_index in range(rounds):
        pipeline = client.pipeline(transaction=False)
        for uuid, registration_key in records:
            if kind == "update":
                arguments = update_arguments(uuid, revision + round_index + 1, bytes([round_index % 2]))
                script_kind = "update"
            elif kind == "update_version":
                arguments = update_arguments(
                    uuid,
                    revision + round_index + 1,
                    bytes([round_index % 2]),
                    1 + round_index % 2,
                )
                script_kind = "update"
            elif kind == "update_wide":
                arguments = update_arguments(
                    uuid,
                    revision + round_index + 1,
                    bytes([round_index % 2]),
                    wide=True,
                )
                script_kind = "update"
            elif kind == "renew":
                arguments = renew_arguments(uuid, revision)
                script_kind = "renew"
            else:
                raise ValueError(kind)
            scripts.enqueue(pipeline, script_kind, registration_key, registry_key, arguments)
        replies = pipeline.execute()
        assert_ok(replies)
        calls += len(replies)
    elapsed = time.perf_counter() - started

    command = client.info("commandstats").get("cmdstat_evalsha", {})
    observed_calls = int(command.get("calls", 0))
    if observed_calls != calls:
        raise AssertionError(f"EVALSHA calls={observed_calls}, expected={calls}")
    return {
        "operation": kind,
        "calls": calls,
        "wall_seconds": elapsed,
        "wall_operations_per_second": calls / elapsed,
        "server_total_microseconds": int(command.get("usec", 0)),
        "server_microseconds_per_call": float(command.get("usec_per_call", 0.0)),
    }


def execute_unregister_phase(
    client: redis.Redis,
    scripts: ScriptSet,
    records: list[tuple[str, str]],
    registry_key: str,
) -> dict[str, Any]:
    client.execute_command("CONFIG", "RESETSTAT")
    pipeline = client.pipeline(transaction=False)
    started = time.perf_counter()
    for uuid, registration_key in records:
        scripts.enqueue(
            pipeline,
            "unregister",
            registration_key,
            registry_key,
            unregister_arguments(uuid),
        )
    replies = pipeline.execute()
    elapsed = time.perf_counter() - started
    assert_ok(replies)

    command = client.info("commandstats").get("cmdstat_evalsha", {})
    observed_calls = int(command.get("calls", 0))
    if observed_calls != len(records):
        raise AssertionError(f"EVALSHA calls={observed_calls}, expected={len(records)}")
    return {
        "operation": "unregister",
        "calls": len(records),
        "wall_seconds": elapsed,
        "wall_operations_per_second": len(records) / elapsed,
        "server_total_microseconds": int(command.get("usec", 0)),
        "server_microseconds_per_call": float(command.get("usec_per_call", 0.0)),
    }


def run_mode(
    client: redis.Redis,
    scripts: ScriptSet,
    variant: str,
    registrations: int,
    rounds: int,
    trial: int,
) -> list[dict[str, Any]]:
    zone = "LuaBench" + "".join(secrets.choice(string.ascii_letters) for _ in range(12))
    registry_key = f"verdandi:registry:{zone}:proxy"
    records = [
        (
            f"{index + 1:032x}",
            f"verdandi:registration:{zone}:proxy:{index + 1:032x}",
        )
        for index in range(registrations)
    ]

    try:
        pipeline = client.pipeline(transaction=False)
        for uuid, registration_key in records:
            scripts.enqueue(
                pipeline,
                "register",
                registration_key,
                registry_key,
                register_arguments(uuid),
            )
        assert_ok(pipeline.execute())

        update = execute_phase(client, scripts, "update", records, registry_key, rounds, 1)
        update_version = execute_phase(client, scripts, "update_version", records, registry_key, rounds, rounds + 1)
        update_wide = execute_phase(client, scripts, "update_wide", records, registry_key, rounds, 2 * rounds + 1)
        renew = execute_phase(client, scripts, "renew", records, registry_key, rounds, 3 * rounds + 1)
        unregister = execute_unregister_phase(client, scripts, records, registry_key)
        for result in (update, update_version, update_wide, renew, unregister):
            result["trial"] = trial
            result["variant"] = variant
        return [update, update_version, update_wide, renew, unregister]
    finally:
        pipeline = client.pipeline(transaction=False)
        for _, registration_key in records:
            pipeline.unlink(registration_key)
        pipeline.execute()
        client.delete(registry_key)


def summaries(results: list[dict[str, Any]], variants: tuple[str, ...]) -> list[dict[str, Any]]:
    output = []
    for operation in (
        "update",
        "update_version",
        "update_wide",
        "renew",
        "unregister",
    ):
        baseline = {result["trial"]: result for result in results if result["operation"] == operation and result["variant"] == "baseline"}
        baseline_server = statistics.median(result["server_microseconds_per_call"] for result in baseline.values())
        baseline_wall = statistics.median(result["wall_operations_per_second"] for result in baseline.values())
        for variant in variants:
            selected = [result for result in results if result["operation"] == operation and result["variant"] == variant]
            paired = [
                (baseline[result["trial"]]["server_microseconds_per_call"] - result["server_microseconds_per_call"])
                / baseline[result["trial"]]["server_microseconds_per_call"]
                * 100
                for result in selected
            ]
            server = statistics.median(result["server_microseconds_per_call"] for result in selected)
            wall = statistics.median(result["wall_operations_per_second"] for result in selected)
            output.append(
                {
                    "operation": operation,
                    "variant": variant,
                    "trials": len(selected),
                    "median_server_microseconds_per_call": server,
                    "server_improvement_percent": (baseline_server - server) / baseline_server * 100,
                    "paired_server_improvement_median_percent": statistics.median(paired),
                    "paired_server_improvement_positive_trials": sum(improvement > 0 for improvement in paired),
                    "paired_server_improvement_min_percent": min(paired),
                    "paired_server_improvement_max_percent": max(paired),
                    "median_wall_operations_per_second": wall,
                    "wall_improvement_percent": (wall / baseline_wall - 1) * 100,
                }
            )
    return output


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--redis-url",
        default=os.environ.get("VERDANDI_REDIS_URL", "redis://127.0.0.1:6379/0"),
    )
    parser.add_argument("--registrations", type=int, default=500)
    parser.add_argument("--rounds", type=int, default=20)
    parser.add_argument("--trials", type=int, default=11)
    parser.add_argument("--baseline-update-sha")
    parser.add_argument("--baseline-renew-sha")
    parser.add_argument("--baseline-unregister-sha")
    parser.add_argument("--output", type=Path)
    arguments = parser.parse_args()
    if arguments.registrations < 1 or arguments.rounds < 1 or arguments.trials < 1:
        parser.error("registrations, rounds, and trials must be positive")

    client = redis.Redis.from_url(arguments.redis_url, decode_responses=False)
    server = client.info("server")
    version = tuple(int(part) for part in server["redis_version"].split(".")[:2])
    if version < (8, 0):
        raise RuntimeError(f"Redis 8.0 or later is required, found {version!r}")

    baseline_options = (
        arguments.baseline_update_sha,
        arguments.baseline_renew_sha,
        arguments.baseline_unregister_sha,
    )
    if any(baseline_options) and not all(baseline_options):
        parser.error("all three baseline SHA options must be supplied together")

    candidate = ScriptSet(client)
    if all(baseline_options):
        baseline = ScriptSet(
            client,
            {
                "update": arguments.baseline_update_sha,
                "renew": arguments.baseline_renew_sha,
                "unregister": arguments.baseline_unregister_sha,
            },
        )
        script_sets = {"baseline": baseline, "candidate": candidate}
    else:
        script_sets = {"baseline": candidate}

    results: list[dict[str, Any]] = []
    for trial in range(1, arguments.trials + 1):
        names = list(script_sets)
        if trial % 2 == 0:
            names.reverse()
        for name in names:
            results.extend(
                run_mode(
                    client,
                    script_sets[name],
                    name,
                    arguments.registrations,
                    arguments.rounds,
                    trial,
                )
            )
    report = {
        "redis_version": server["redis_version"],
        "registrations": arguments.registrations,
        "rounds": arguments.rounds,
        "trials": arguments.trials,
        "subscribers": 0,
        "persistence": "fixture-disabled",
        "variants": list(script_sets),
        "results": results,
        "summary": summaries(results, tuple(script_sets)),
    }
    encoded = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if arguments.output:
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.write_text(encoded, encoding="utf-8", newline="\n")
    print(encoded, end="")


if __name__ == "__main__":
    main()
