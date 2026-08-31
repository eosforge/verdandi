#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 LaconisIves

"""Exercise the Catalog Hash, ZSET, revision, and Pub/Sub Lua contract."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import secrets
import string
import threading
from concurrent.futures import ThreadPoolExecutor
from typing import Any, Iterable

import msgpack
import redis

import generate_catalog

ROOT = Path(__file__).resolve().parents[2]
SCRIPT_ROOT = ROOT / "lua" / "catalog"
SCRIPT_KINDS = ("read", "replace", "patch", "delete")
MAX_SAFE_INTEGER = 9_007_199_254_740_991


def decoded_pairs(items: Iterable[Any]) -> dict[bytes, Any]:
    values = list(items)
    assert len(values) % 2 == 0, values
    result: dict[bytes, Any] = {}
    for index in range(0, len(values), 2):
        field = values[index]
        assert isinstance(field, bytes), values
        assert field not in result, values
        result[field] = values[index + 1]
    return result


def expect_status(
    reply: Iterable[Any],
    result: bytes,
    status: bytes | None = None,
) -> dict[bytes, Any]:
    values = decoded_pairs(reply)
    assert values[b"&result"] == result, values
    if status is not None:
        assert values[b"&status"] == status, values
    return values


def encoded_size(fields: list[tuple[str, bytes]]) -> int:
    return sum(len(name.encode()) + len(value) for name, value in fields)


class CatalogScripts:
    def __init__(
        self,
        client: redis.Redis,
        zone: str,
        part: str,
        catalog_id: str,
    ) -> None:
        self.client = client
        self.sources = {kind: (SCRIPT_ROOT / f"{kind}.lua").read_text(encoding="utf-8") for kind in SCRIPT_KINDS}
        self.shas = {kind: client.script_load(source) for kind, source in self.sources.items()}
        self.zone = zone
        self.member = f"{part}:{catalog_id}"
        self.meta = f"verdandi:catalog:{zone}:@meta"
        self.live = f"verdandi:catalog:{zone}:@live"
        self.deleted = f"verdandi:catalog:{zone}:@deleted"
        self.deleted_time = f"verdandi:catalog:{zone}:@deleted_time"
        self.key = f"verdandi:catalog:{zone}:{part}:{catalog_id}"
        self.field_revisions = f"{self.key}:@field_revisions"

    @property
    def mutation_keys(self) -> list[str]:
        return [
            self.meta,
            self.live,
            self.deleted,
            self.deleted_time,
            self.key,
            self.field_revisions,
        ]

    def call(self, kind: str, keys: list[str], *arguments: Any) -> list[Any]:
        try:
            return self.client.evalsha(self.shas[kind], len(keys), *keys, *arguments)
        except redis.exceptions.NoScriptError:
            self.shas[kind] = self.client.script_load(self.sources[kind])
            return self.client.evalsha(self.shas[kind], len(keys), *keys, *arguments)

    def replace(
        self,
        kind: str,
        fields: list[tuple[str, bytes]],
    ) -> dict[bytes, Any]:
        arguments: list[Any] = [
            self.member,
            kind,
            str(encoded_size(fields)),
            str(len(fields)),
        ]
        for name, value in fields:
            arguments.extend((name, value))
        return decoded_pairs(self.call("replace", self.mutation_keys, *arguments))

    def patch(
        self,
        base_revision: int,
        projected_bytes: int,
        fields: list[tuple[str, bytes]],
    ) -> dict[bytes, Any]:
        arguments: list[Any] = [
            self.member,
            str(base_revision),
            str(projected_bytes),
            str(len(fields)),
        ]
        for name, value in fields:
            arguments.extend((name, value))
        return decoded_pairs(self.call("patch", self.mutation_keys, *arguments))

    def delete(self) -> dict[bytes, Any]:
        return decoded_pairs(self.call("delete", self.mutation_keys, self.member))

    def read(self, local_revision: int = 0) -> dict[bytes, Any]:
        return decoded_pairs(
            self.call(
                "read",
                [
                    self.live,
                    self.deleted,
                    self.deleted_time,
                    self.key,
                    self.field_revisions,
                ],
                self.member,
                str(local_revision),
            )
        )


def next_event(pubsub: redis.client.PubSub) -> list[Any]:
    while True:
        message = pubsub.get_message(timeout=2.0)
        assert message is not None, "timed out waiting for Catalog notification"
        if message["type"] in ("message", "pmessage"):
            return msgpack.unpackb(message["data"], raw=True)


def exercise_catalog(client: redis.Redis, scripts: CatalogScripts) -> None:
    pubsub = client.pubsub(ignore_subscribe_messages=False)
    pubsub.subscribe(scripts.key)
    subscribed = pubsub.get_message(timeout=2.0)
    assert subscribed is not None and subscribed["type"] == "subscribe"
    try:
        fields = [("0", b"first"), ("1", b"second"), ("2", b"third")]
        created = scripts.replace("array", fields)
        assert created == {
            b"&result": b"ok",
            b"@revision": b"1",
            b"@floor_revision": b"0",
        }
        assert client.hgetall(scripts.key) == {
            b"@revision": b"1",
            b"@replace_revision": b"1",
            b"@kind": b"array",
            b"@encoded_bytes": str(encoded_size(fields)).encode(),
            b"0": b"first",
            b"1": b"second",
            b"2": b"third",
        }
        assert client.zscore(scripts.live, scripts.member) == 1.0
        assert client.zrange(scripts.field_revisions, 0, -1, withscores=True) == [
            (b"0", 1.0),
            (b"1", 1.0),
            (b"2", 1.0),
        ]
        assert next_event(pubsub) == [
            b"v1",
            b"replace",
            scripts.member.encode(),
            b"1",
            b"array",
            str(encoded_size(fields)).encode(),
            [b"0", b"first", b"1", b"second", b"2", b"third"],
        ]
        snapshot = scripts.read()
        assert snapshot[b"&status"] == b"present"
        assert snapshot[b"&mode"] == b"replace"
        assert snapshot[b"@revision"] == b"1"
        assert snapshot[b"&fields"] == [b"0", b"first", b"1", b"second", b"2", b"third"]

        projected = encoded_size([("0", b"first"), ("1", b"changed"), ("2", b"third")])
        changed = scripts.patch(1, projected, [("1", b"changed")])
        assert changed[b"@revision"] == b"2"
        assert client.zscore(scripts.field_revisions, "1") == 2.0
        assert next_event(pubsub) == [
            b"v1",
            b"patch",
            scripts.member.encode(),
            b"1",
            b"2",
            b"array",
            str(projected).encode(),
            [b"1", b"changed"],
        ]
        delta = scripts.read(1)
        assert delta[b"&mode"] == b"patch"
        assert delta[b"@revision"] == b"2"
        assert delta[b"&fields"] == [b"1", b"changed"]
        unchanged = scripts.read(2)
        assert unchanged[b"&mode"] == b"unchanged"
        assert unchanged[b"&fields"] == []

        stale = scripts.patch(1, projected, [("1", b"stale")])
        assert stale[b"&result"] == b"error"
        assert stale[b"&status"] == b"stale"
        assert stale[b"@revision"] == b"2"

        append = scripts.patch(2, projected + 2, [("3", b"x")])
        assert append[b"&status"] == b"transition"

        map_fields = [("primary", b"east"), ("weight", b"10")]
        replaced = scripts.replace("map", map_fields)
        assert replaced[b"@revision"] == b"3"
        assert client.hmget(scripts.key, "0", "primary") == [None, b"east"]
        assert next_event(pubsub)[1] == b"replace"

        map_projected = encoded_size(map_fields + [("zone", b"west")])
        patched = scripts.patch(
            3,
            map_projected,
            [("weight", b"10"), ("zone", b"west")],
        )
        assert patched[b"@revision"] == b"4"
        assert next_event(pubsub)[1] == b"patch"

        value_fields = [("value", b"opaque\x00bytes")]
        value_result = scripts.replace("value", value_fields)
        assert value_result[b"@revision"] == b"5"
        assert next_event(pubsub)[4] == b"value"

        invalid_patch = scripts.patch(
            5,
            encoded_size(value_fields),
            [("value", b"next")],
        )
        assert invalid_patch[b"&status"] == b"transition"

        deleted = scripts.delete()
        assert deleted[b"@revision"] == b"6"
        assert client.exists(scripts.key, scripts.field_revisions) == 0
        assert client.zscore(scripts.live, scripts.member) is None
        assert client.zscore(scripts.deleted, scripts.member) == 6.0
        assert next_event(pubsub) == [
            b"v1",
            b"delete",
            scripts.member.encode(),
            b"6",
        ]
        tombstone = scripts.read(5)
        assert tombstone == {
            b"&result": b"ok",
            b"&status": b"deleted",
            b"@revision": b"6",
        }

        repeated = scripts.delete()
        assert repeated[b"@revision"] == b"7"
        assert client.zscore(scripts.deleted, scripts.member) == 7.0
        assert next_event(pubsub)[3] == b"7"

        restored = scripts.replace("map", [])
        assert restored[b"@revision"] == b"8"
        assert client.zscore(scripts.deleted, scripts.member) is None
        assert client.hlen(scripts.key) == 4
        assert next_event(pubsub)[1] == b"replace"
        empty = scripts.read()
        assert empty[b"&mode"] == b"replace"
        assert empty[b"&fields"] == []
    finally:
        pubsub.close()


def exercise_patch_contention(scripts: CatalogScripts) -> None:
    """Require one exact-base winner when two writers race without a lease."""
    assert scripts.replace("map", [("a", b"one")])[b"@revision"] == b"1"
    barrier = threading.Barrier(2)

    def patch(value: bytes) -> dict[bytes, Any]:
        barrier.wait(timeout=2.0)
        return scripts.patch(1, len("a") + len(value), [("a", value)])

    with ThreadPoolExecutor(max_workers=2) as executor:
        replies = list(executor.map(patch, (b"two", b"six")))

    successes = [reply for reply in replies if reply[b"&result"] == b"ok"]
    stale = [reply for reply in replies if reply.get(b"&status") == b"stale"]
    assert len(successes) == 1, replies
    assert len(stale) == 1, replies
    assert successes[0][b"@revision"] == b"2"
    assert stale[0][b"@revision"] == b"2"
    state = scripts.read()
    assert state[b"@revision"] == b"2"
    assert state[b"&fields"] in ([b"a", b"two"], [b"a", b"six"])


def exercise_encoded_bytes_ceiling(client: redis.Redis, scripts: CatalogScripts) -> None:
    payload = b"x" * (512 * 1024)
    accepted = scripts.replace("value", [("value", payload)])
    assert accepted[b"&result"] == b"ok"
    assert client.hstrlen(scripts.key, "value") == len(payload)

    rejected = decoded_pairs(
        scripts.call(
            "replace",
            scripts.mutation_keys,
            scripts.member,
            "value",
            str(4 * 1024 * 1024 + 1),
            "1",
            "value",
            b"unchanged",
        )
    )
    assert rejected[b"&result"] == b"error"
    assert rejected[b"&status"] == b"contract", rejected
    assert client.hstrlen(scripts.key, "value") == len(payload)


def exercise_safe_integer_boundary(client: redis.Redis, scripts: CatalogScripts) -> None:
    before = MAX_SAFE_INTEGER - 1
    fields = [("value", b"before")]
    client.hset(
        scripts.meta,
        mapping={"@revision": str(before), "@floor_revision": "0"},
    )
    client.hset(
        scripts.key,
        mapping={
            "@revision": str(before),
            "@replace_revision": str(before),
            "@kind": "value",
            "@encoded_bytes": str(encoded_size(fields)),
            "value": b"before",
        },
    )
    client.zadd(scripts.live, {scripts.member: before})
    client.zadd(scripts.field_revisions, {"value": before})

    maximum = scripts.replace("value", [("value", b"maximum")])
    assert maximum[b"@revision"] == str(MAX_SAFE_INTEGER).encode()

    overflow = scripts.replace("value", [("value", b"overflow")])
    assert overflow[b"&status"] == b"capacity"
    assert client.hget(scripts.key, "value") == b"maximum"


def exercise_floor_advances_only_on_eviction(
    client: redis.Redis,
    scripts: CatalogScripts,
) -> None:
    old_members = {
        "old:deleted-a": 1,
        "old:deleted-b": 2,
        "old:deleted-c": 3,
    }
    client.hset(
        scripts.meta,
        mapping={"@revision": "3", "@floor_revision": "0"},
    )
    client.zadd(scripts.deleted, old_members)
    client.zadd(scripts.deleted_time, dict.fromkeys(old_members, 0))

    deleted = scripts.delete()
    assert deleted[b"@revision"] == b"4"
    assert deleted[b"@floor_revision"] == b"3"
    assert deleted[b"@pruned"] == b"3"
    assert all(client.zscore(scripts.deleted, member) is None for member in old_members)
    assert client.zscore(scripts.deleted, scripts.member) == 4.0


def exercise_corruption_guard(client: redis.Redis, scripts: CatalogScripts) -> None:
    client.set(scripts.meta, "wrong-type")
    corrupted = scripts.delete()
    assert corrupted[b"&result"] == b"error"
    assert corrupted[b"&status"] == b"corrupt"


def exercise_strict_abi_and_missing_field_index(
    client: redis.Redis,
    scripts: CatalogScripts,
) -> None:
    assert (
        expect_status(
            scripts.call(
                "replace",
                [*scripts.mutation_keys, scripts.meta],
                scripts.member,
                "map",
                "4",
                "1",
                "a",
                b"one",
            ),
            b"error",
            b"contract",
        )[b"&status"]
        == b"contract"
    )
    assert scripts.replace("map", [("a", b"one")])[b"@revision"] == b"1"
    client.unlink(scripts.field_revisions)
    assert (
        expect_status(
            scripts.call(
                "read",
                [
                    scripts.live,
                    scripts.deleted,
                    scripts.deleted_time,
                    scripts.key,
                    scripts.field_revisions,
                    scripts.meta,
                ],
                scripts.member,
                "0",
            ),
            b"error",
            b"contract",
        )[b"&status"]
        == b"contract"
    )
    assert scripts.read()[b"&status"] == b"corrupt"

    corrupted = scripts.patch(1, 4, [("a", b"two")])
    assert corrupted[b"&result"] == b"error"
    assert corrupted[b"&status"] == b"corrupt"


def cleanup_zone(client: redis.Redis, zone: str) -> None:
    pattern = f"verdandi:catalog:{zone}:*"
    keys = list(client.scan_iter(match=pattern, count=256))
    if keys:
        client.unlink(*keys)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--redis-url",
        default=os.environ.get("VERDANDI_REDIS_URL", "redis://127.0.0.1:6379/0"),
    )
    arguments = parser.parse_args()

    generated = generate_catalog.expected_outputs(generate_catalog.load_manifest())
    if generate_catalog.check(generated) != 0:
        raise RuntimeError("Catalog Lua generated files are stale")

    client = redis.Redis.from_url(arguments.redis_url, decode_responses=False)
    version = tuple(int(part) for part in client.info("server")["redis_version"].split(".")[:2])
    if version < (8, 0):
        raise RuntimeError(f"Redis 8.0 or later is required, found {version!r}")

    zone = "CatalogTest" + "".join(secrets.choice(string.ascii_letters) for _ in range(12))
    try:
        exercise_catalog(client, CatalogScripts(client, zone, "routing", "routes"))
        cleanup_zone(client, zone)
        exercise_patch_contention(
            CatalogScripts(client, zone, "routing", "contention"),
        )
        cleanup_zone(client, zone)
        exercise_encoded_bytes_ceiling(
            client,
            CatalogScripts(client, zone, "boundary", "encoded-bytes"),
        )
        cleanup_zone(client, zone)
        exercise_safe_integer_boundary(
            client,
            CatalogScripts(client, zone, "boundary", "maximum"),
        )
        cleanup_zone(client, zone)
        exercise_floor_advances_only_on_eviction(
            client,
            CatalogScripts(client, zone, "floor", "target"),
        )
        cleanup_zone(client, zone)
        exercise_corruption_guard(
            client,
            CatalogScripts(client, zone, "corrupt", "wrongtype"),
        )
        cleanup_zone(client, zone)
        exercise_strict_abi_and_missing_field_index(
            client,
            CatalogScripts(client, zone, "strict", "index"),
        )
    finally:
        cleanup_zone(client, zone)

    redis_version = client.info("server")["redis_version"]
    print(
        "PASS Catalog Hash/ZSET revision, exact-base contention, 4-MiB ceiling, Replace/Patch/Delete, "
        f"Pub/Sub, floor, and 2^53-1 on Redis {redis_version}"
    )


if __name__ == "__main__":
    main()
