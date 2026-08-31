#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 LaconisIves

"""Measure the current generated Registration Register script on Redis 8."""

from __future__ import annotations

import argparse
from collections.abc import Iterable
import json
import os
from pathlib import Path
import secrets
import statistics
import string
import time
from typing import Any

import redis

ROOT = Path(__file__).resolve().parents[2]
GENERATED = ROOT / "lua" / "registration" / "register.lua"
TTL_MS = 120_000
MAX_SAFE_INTEGER = 9_007_199_254_740_991


def candidate_source() -> str:
    return GENERATED.read_text(encoding="utf-8")


def decoded_pairs(reply: Iterable[Any]) -> dict[bytes, Any]:
    values = list(reply)
    if len(values) % 2 != 0:
        raise AssertionError(f"odd reply: {values!r}")
    return dict(zip(values[::2], values[1::2], strict=True))


def payload(shape: str) -> list[Any]:
    if shape == "small":
        return [
            ".build",
            b"benchmark",
            ".region",
            b"local",
            "address",
            b"127.0.0.1:8080",
            "load",
            b"0",
        ]
    if shape == "default_max":
        fields: list[Any] = []
        for index in range(16):
            fields.extend((f".a{index:02d}", b"a" * 128))
        for index in range(32):
            fields.extend((f"d{index:02d}", b"d" * 128))
        return fields
    raise ValueError(shape)


def register_arguments(uuid: str, application: list[Any]) -> list[Any]:
    return [
        uuid,
        1,
        TTL_MS,
        1,
        *application,
    ]


def cleanup(client: redis.Redis, registration_keys: list[str], registry_key: str) -> None:
    pipeline = client.pipeline(transaction=False)
    for offset in range(0, len(registration_keys), 100):
        pipeline.unlink(*registration_keys[offset : offset + 100])
    pipeline.delete(registry_key)
    pipeline.execute()


def validate_candidate(client: redis.Redis, name: str, sha: str) -> None:
    zone = "LuaLineCheck" + "".join(secrets.choice(string.ascii_letters) for _ in range(8))
    uuid = secrets.token_hex(16)
    registration_key = f"verdandi:registration:{zone}:proxy:{uuid}"
    registry_key = f"verdandi:registry:{zone}:proxy"
    application = payload("small")
    try:
        reply = client.evalsha(
            sha,
            2,
            registration_key,
            registry_key,
            *register_arguments(uuid, application),
        )
        decoded = decoded_pairs(reply)
        if decoded.get(b"&result") != b"ok":
            raise AssertionError(f"{name}: reply={decoded!r}")
        stored = client.hgetall(registration_key)
        expected = {
            b"@uuid": uuid.encode(),
            b"@revision": b"1",
            b"@ttl": str(TTL_MS).encode(),
            b"@version": b"1",
        }
        expected.update((str(application[index]).encode(), application[index + 1]) for index in range(0, len(application), 2))
        for field, value in expected.items():
            if stored.get(field) != value:
                raise AssertionError(f"{name}: stored {field!r}={stored.get(field)!r}, expected={value!r}")
        timestamp = stored.get(b"@timestamp", b"")
        if not timestamp.isdigit() or int(timestamp) <= 0:
            raise AssertionError(f"{name}: noncanonical timestamp {timestamp!r}")
        if client.hget(registry_key, uuid) != b"1":
            raise AssertionError(f"{name}: membership revision mismatch")
        registration_ttl = client.pttl(registration_key)
        membership_ttl = client.execute_command("HPTTL", registry_key, "FIELDS", 1, uuid)[0]
        if not 0 < registration_ttl <= TTL_MS or not 0 < membership_ttl <= TTL_MS:
            raise AssertionError(f"{name}: ttl mismatch registration={registration_ttl} membership={membership_ttl}")
    finally:
        cleanup(client, [registration_key], registry_key)


def discover_hash_field_expire_at_max(client: redis.Redis) -> int:
    key = "verdandi:lua:hfe-limit:" + secrets.token_hex(8)
    field = "member"
    low = int(time.time() * 1_000) + 1
    high = MAX_SAFE_INTEGER

    def accepted(expire_at_ms: int) -> bool:
        client.delete(key)
        client.hset(key, field, "1")
        try:
            reply = client.execute_command("HPEXPIREAT", key, expire_at_ms, "FIELDS", 1, field)
        except redis.ResponseError as error:
            if "invalid expire time" in str(error):
                return False
            raise
        return reply == [1]

    try:
        while low < high:
            middle = (low + high + 1) // 2
            if accepted(middle):
                low = middle
            else:
                high = middle - 1
        return low
    finally:
        client.delete(key)


def validate_numeric_arguments(client: redis.Redis, hash_field_expire_at_max_ms: int) -> dict[str, Any]:
    key = "verdandi:lua:numeric-check:" + secrets.token_hex(8)
    source = """
local safe_integer = tonumber(ARGV[1])
local field_expire_at = tonumber(ARGV[2])
redis.call("HSET", KEYS[1], "number", safe_integer, "member", "1")
redis.call("HPEXPIREAT", KEYS[1], field_expire_at, "FIELDS", 1, "member")
redis.call("PEXPIREAT", KEYS[1], safe_integer)
local field_expiry = redis.call("HPEXPIRETIME", KEYS[1], "FIELDS", 1, "member")
return {
    redis.call("HGET", KEYS[1], "number"),
    redis.call("PEXPIRETIME", KEYS[1]),
    field_expiry[1],
}
"""
    try:
        reply = client.evalsha(
            client.script_load(source),
            1,
            key,
            str(MAX_SAFE_INTEGER),
            str(hash_field_expire_at_max_ms),
        )
        expected = [
            str(MAX_SAFE_INTEGER).encode(),
            MAX_SAFE_INTEGER,
            hash_field_expire_at_max_ms,
        ]
        if reply != expected:
            raise AssertionError(f"numeric Redis argument mismatch: {reply!r}")

        max_safe_rejected = False
        client.hset(key, "member", "1")
        try:
            client.execute_command("HPEXPIREAT", key, MAX_SAFE_INTEGER, "FIELDS", 1, "member")
        except redis.ResponseError as error:
            if "invalid expire time" not in str(error):
                raise
            max_safe_rejected = True
        if not max_safe_rejected:
            raise AssertionError("HPEXPIREAT unexpectedly accepted the Lua safe integer maximum")

        hsetex_field = "member_hsetex"
        hsetex_reply = client.execute_command(
            "HSETEX",
            key,
            "PXAT",
            hash_field_expire_at_max_ms,
            "FIELDS",
            1,
            hsetex_field,
            "2",
        )
        if hsetex_reply != 1:
            raise AssertionError(f"HSETEX ceiling reply mismatch: {hsetex_reply!r}")
        hsetex_expiry = client.execute_command("HPEXPIRETIME", key, "FIELDS", 1, hsetex_field)
        if hsetex_expiry != [hash_field_expire_at_max_ms]:
            raise AssertionError(f"HSETEX ceiling expiry mismatch: {hsetex_expiry!r}")

        hsetex_plus_one_rejected = False
        try:
            client.execute_command(
                "HSETEX",
                key,
                "PXAT",
                hash_field_expire_at_max_ms + 1,
                "FIELDS",
                1,
                "member_hsetex_plus_one",
                "3",
            )
        except redis.ResponseError as error:
            if "invalid expire time" not in str(error):
                raise
            hsetex_plus_one_rejected = True
        if not hsetex_plus_one_rejected:
            raise AssertionError("HSETEX unexpectedly accepted the field-expiry ceiling plus one")

        return {
            "status": "pass",
            "safe_integer": MAX_SAFE_INTEGER,
            "stored_hash": int(reply[0]),
            "key_expire_at_ms": reply[1],
            "field_expire_at_ms": reply[2],
            "hash_field_expire_at_max_ms": hash_field_expire_at_max_ms,
            "hsetex_field_expire_at_ms": hsetex_expiry[0],
            "hsetex_ceiling_plus_one_rejected": hsetex_plus_one_rejected,
            "safe_integer_rejected_for_hash_field_expiry": max_safe_rejected,
        }
    finally:
        client.delete(key)


def execute_phase(
    client: redis.Redis,
    name: str,
    sha: str,
    shape: str,
    registrations: int,
    trial: int,
) -> dict[str, Any]:
    zone = "LuaLine" + "".join(secrets.choice(string.ascii_letters) for _ in range(12))
    registry_key = f"verdandi:registry:{zone}:proxy"
    records = [
        (
            f"{index + 1:032x}",
            f"verdandi:registration:{zone}:proxy:{index + 1:032x}",
        )
        for index in range(registrations)
    ]
    registration_keys = [registration_key for _, registration_key in records]
    application = payload(shape)
    try:
        client.execute_command("CONFIG", "RESETSTAT")
        pipeline = client.pipeline(transaction=False)
        started = time.perf_counter()
        for uuid, registration_key in records:
            pipeline.evalsha(
                sha,
                2,
                registration_key,
                registry_key,
                *register_arguments(uuid, application),
            )
        replies = pipeline.execute()
        elapsed = time.perf_counter() - started
        for reply in replies:
            if decoded_pairs(reply).get(b"&result") != b"ok":
                raise AssertionError(f"{name}/{shape}: failed reply {reply!r}")

        command = client.info("commandstats").get("cmdstat_evalsha", {})
        observed_calls = int(command.get("calls", 0))
        if observed_calls != registrations:
            raise AssertionError(f"{name}/{shape}: EVALSHA calls={observed_calls}, expected={registrations}")
        return {
            "variant": name,
            "shape": shape,
            "trial": trial,
            "calls": registrations,
            "wall_seconds": elapsed,
            "wall_operations_per_second": registrations / elapsed,
            "server_total_microseconds": int(command.get("usec", 0)),
            "server_microseconds_per_call": float(command.get("usec_per_call", 0.0)),
        }
    finally:
        cleanup(client, registration_keys, registry_key)


def summaries(results: list[dict[str, Any]], source_bytes: dict[str, int]) -> list[dict[str, Any]]:
    output = []
    for shape in ("small", "default_max"):
        baseline = [result for result in results if result["variant"] == "baseline" and result["shape"] == shape]
        baseline_server = statistics.median(result["server_microseconds_per_call"] for result in baseline)
        baseline_wall = statistics.median(result["wall_operations_per_second"] for result in baseline)
        baseline_by_trial = {result["trial"]: result["server_microseconds_per_call"] for result in baseline}
        for name, byte_count in source_bytes.items():
            selected = [result for result in results if result["variant"] == name and result["shape"] == shape]
            server = statistics.median(result["server_microseconds_per_call"] for result in selected)
            wall = statistics.median(result["wall_operations_per_second"] for result in selected)
            paired = [
                (baseline_by_trial[result["trial"]] - result["server_microseconds_per_call"]) / baseline_by_trial[result["trial"]] * 100 for result in selected
            ]
            parent_name = None if name == "baseline" else "baseline"
            parent_by_trial = baseline_by_trial
            if parent_name is not None and parent_name != "baseline":
                parent_by_trial = {
                    result["trial"]: result["server_microseconds_per_call"]
                    for result in results
                    if result["variant"] == parent_name and result["shape"] == shape
                }
            paired_parent = [
                (parent_by_trial[result["trial"]] - result["server_microseconds_per_call"]) / parent_by_trial[result["trial"]] * 100 for result in selected
            ]
            output.append(
                {
                    "variant": name,
                    "parent_variant": parent_name,
                    "shape": shape,
                    "source_bytes": byte_count,
                    "trials": len(selected),
                    "median_server_microseconds_per_call": server,
                    "server_improvement_percent": (baseline_server - server) / baseline_server * 100,
                    "paired_server_improvement_median_percent": statistics.median(paired),
                    "paired_server_improvement_positive_trials": sum(improvement > 0 for improvement in paired),
                    "paired_server_improvement_min_percent": min(paired),
                    "paired_server_improvement_max_percent": max(paired),
                    "paired_parent_improvement_median_percent": statistics.median(paired_parent),
                    "paired_parent_improvement_positive_trials": sum(improvement > 0 for improvement in paired_parent),
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
    parser.add_argument("--small-registrations", type=int, default=2_000)
    parser.add_argument("--default-max-registrations", type=int, default=500)
    parser.add_argument("--trials", type=int, default=11)
    parser.add_argument(
        "--baseline-sha",
        help="cached pre-change Register SHA for a paired candidate comparison",
    )
    parser.add_argument("--baseline-source-bytes", type=int)
    parser.add_argument("--output", type=Path)
    arguments = parser.parse_args()
    if (
        min(
            arguments.small_registrations,
            arguments.default_max_registrations,
            arguments.trials,
        )
        < 1
    ):
        parser.error("registration counts and trials must be positive")

    client = redis.Redis.from_url(arguments.redis_url, decode_responses=False)
    if client.dbsize() != 0:
        raise RuntimeError("registration line benchmark requires an empty isolated database")
    server = client.info("server")
    version = tuple(int(part) for part in server["redis_version"].split(".")[:2])
    if version < (8, 0):
        raise RuntimeError(f"Redis 8.0 or later is required, found {version!r}")

    source = candidate_source()
    if arguments.baseline_sha:
        if arguments.baseline_source_bytes is None or arguments.baseline_source_bytes < 1:
            parser.error("--baseline-source-bytes is required with --baseline-sha")
        if client.script_exists(arguments.baseline_sha) != [True]:
            raise RuntimeError("the cached baseline Register SHA is unavailable")
        shas = {
            "baseline": arguments.baseline_sha,
            "candidate": client.script_load(source),
        }
        source_bytes = {
            "baseline": arguments.baseline_source_bytes,
            "candidate": len(source.encode("utf-8")),
        }
    else:
        shas = {"baseline": client.script_load(source)}
        source_bytes = {"baseline": len(source.encode("utf-8"))}
    for name, sha in shas.items():
        validate_candidate(client, name, sha)
    hash_field_expire_at_max_ms = discover_hash_field_expire_at_max(client)
    numeric_check = validate_numeric_arguments(client, hash_field_expire_at_max_ms)

    results: list[dict[str, Any]] = []
    names = list(shas)
    shapes = ("small", "default_max")
    counts = {
        "small": arguments.small_registrations,
        "default_max": arguments.default_max_registrations,
    }
    for trial in range(1, arguments.trials + 1):
        offset = (trial - 1) % len(names)
        ordered_names = names[offset:] + names[:offset]
        ordered_shapes = shapes if trial % 2 else tuple(reversed(shapes))
        for shape in ordered_shapes:
            for name in ordered_names:
                results.append(
                    execute_phase(
                        client,
                        name,
                        shas[name],
                        shape,
                        counts[shape],
                        trial,
                    )
                )

    if client.dbsize() != 0:
        raise AssertionError("benchmark cleanup left Redis keys behind")
    report = {
        "redis_version": server["redis_version"],
        "trials": arguments.trials,
        "registrations": counts,
        "subscribers": 0,
        "persistence": "fixture-disabled",
        "numeric_safe_integer_arguments": numeric_check,
        "variants": list(shas),
        "results": results,
        "summary": summaries(results, source_bytes),
    }
    encoded = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if arguments.output:
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.write_text(encoded, encoding="utf-8", newline="\n")
    print(encoded, end="")


if __name__ == "__main__":
    main()
