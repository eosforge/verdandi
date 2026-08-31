#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 LaconisIves

"""Run Registration Lua and initial Selector synchronization against Redis 8."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import secrets
import string
import time
from typing import Any, Iterable

import msgpack
import redis

import generate_registration

ROOT = Path(__file__).resolve().parents[2]
SCRIPT_ROOT = ROOT / "lua" / "registration"
SCRIPT_KINDS = ("register", "update", "renew", "unregister")
TTL_MS = 60_000
MAX_SAFE_INTEGER = 9_007_199_254_740_991


def pairs(*items: Any) -> list[Any]:
    return list(items)


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


def next_event(pubsub: redis.client.PubSub, timeout: float = 2.0) -> list[Any]:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        message = pubsub.get_message(timeout=min(0.1, deadline - time.monotonic()))
        if message and message["type"] == "message":
            event = msgpack.unpackb(message["data"], raw=True)
            assert isinstance(event, list), event
            decoded_pairs(event)
            return event
    raise AssertionError("timed out waiting for Registration event")


def expect_no_event(pubsub: redis.client.PubSub, timeout: float = 0.15) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        message = pubsub.get_message(timeout=min(0.05, deadline - time.monotonic()))
        if message and message["type"] == "message":
            raise AssertionError(f"unexpected Registration event: {message!r}")


def wait_for_subscription(pubsub: redis.client.PubSub) -> None:
    deadline = time.monotonic() + 2.0
    while time.monotonic() < deadline:
        message = pubsub.get_message(timeout=0.1)
        if message and message["type"] == "subscribe":
            return
    raise AssertionError("timed out waiting for SUBSCRIBE acknowledgement")


def wait_until_absent(client: redis.Redis, *keys: str) -> None:
    deadline = time.monotonic() + 2.0
    while time.monotonic() < deadline:
        if not any(client.exists(key) for key in keys):
            return
        time.sleep(0.01)
    raise AssertionError(f"keys did not expire: {keys!r}")


class RegistrationScripts:
    def __init__(self, client: redis.Redis, zone: str, registration_type: str) -> None:
        self.client = client
        self.sources = {kind: (SCRIPT_ROOT / f"{kind}.lua").read_text(encoding="utf-8") for kind in SCRIPT_KINDS}
        self.shas = {kind: client.script_load(source) for kind, source in self.sources.items()}
        self.zone = zone
        self.registration_type = registration_type
        self.registry = f"verdandi:registry:{zone}:{registration_type}"

    def key(self, uuid: str) -> str:
        return f"verdandi:registration:{self.zone}:{self.registration_type}:{uuid}"

    def call(self, script_kind: str, uuid: str, *arguments: Any) -> list[Any]:
        if script_kind not in self.sources:
            raise AssertionError(f"unknown Registration operation: {script_kind!r}")
        return self.call_script(script_kind, uuid, uuid, *arguments)

    def call_script(self, script_kind: str, uuid: str, *arguments: Any) -> list[Any]:
        try:
            return self.client.evalsha(
                self.shas[script_kind],
                2,
                self.key(uuid),
                self.registry,
                *arguments,
            )
        except redis.exceptions.NoScriptError:
            self.shas[script_kind] = self.client.script_load(self.sources[script_kind])
            return self.client.evalsha(
                self.shas[script_kind],
                2,
                self.key(uuid),
                self.registry,
                *arguments,
            )

    def register(
        self,
        uuid: str,
        revision: int,
        *,
        ttl: int = TTL_MS,
        version: int = 1,
        address: bytes = b"10.0.0.1:8080",
        load: bytes | None = b"0",
    ) -> list[Any]:
        arguments = [
            revision,
            ttl,
            version,
            ".build",
            b"2026.08.22",
            ".region",
            b"cn-east",
            "address",
            address,
        ]
        if load is not None:
            arguments.extend(("load", load))
        return self.call("register", uuid, *arguments)


def assert_event(event: list[Any], expected: list[Any]) -> None:
    assert event == expected, f"event mismatch\nactual:   {event!r}\nexpected: {expected!r}"


def exercise_registration(client: redis.Redis, script: RegistrationScripts) -> None:
    uuid = "0123456789abcdef0123456789abcdef"
    registration = script.key(uuid)
    pubsub = client.pubsub()
    pubsub.subscribe(script.registry)
    wait_for_subscription(pubsub)

    try:
        registered = expect_status(script.register(uuid, 1), b"ok")
        timestamp = registered[b"@timestamp"]
        assert registered[b"@revision"] == 1
        assert_event(
            next_event(pubsub),
            [
                b"&protocol",
                b"v1",
                b"&kind",
                b"register",
                b"@uuid",
                uuid.encode(),
                b"@revision",
                1,
                b"@timestamp",
                timestamp,
                b"@ttl",
                TTL_MS,
                b"@version",
                1,
                b".build",
                b"2026.08.22",
                b".region",
                b"cn-east",
                b"address",
                b"10.0.0.1:8080",
                b"load",
                b"0",
            ],
        )

        stored = client.hgetall(registration)
        assert stored[b"@uuid"] == uuid.encode()
        assert stored[b"@revision"] == b"1"
        assert stored[b"@timestamp"] == str(timestamp).encode()
        assert client.hget(script.registry, uuid) == b"1"
        key_ttl = client.pttl(registration)
        field_ttl = client.execute_command("HPTTL", script.registry, "FIELDS", 1, uuid)[0]
        assert 0 < key_ttl <= TTL_MS
        assert 0 < field_ttl <= TTL_MS
        assert abs(key_ttl - field_ttl) < 250

        repeated = expect_status(script.register(uuid, 1), b"ok")
        assert repeated[b"@timestamp"] >= timestamp
        timestamp = repeated[b"@timestamp"]
        assert_event(
            next_event(pubsub),
            [
                b"&protocol",
                b"v1",
                b"&kind",
                b"register",
                b"@uuid",
                uuid.encode(),
                b"@revision",
                1,
                b"@timestamp",
                timestamp,
                b"@ttl",
                TTL_MS,
                b"@version",
                1,
                b".build",
                b"2026.08.22",
                b".region",
                b"cn-east",
                b"address",
                b"10.0.0.1:8080",
                b"load",
                b"0",
            ],
        )

        updated = expect_status(
            script.call(
                "update",
                uuid,
                2,
                2,
                "load",
                b"",
            ),
            b"ok",
        )
        update_timestamp = updated[b"@timestamp"]
        assert_event(
            next_event(pubsub),
            [
                b"&protocol",
                b"v1",
                b"&kind",
                b"update",
                b"@uuid",
                uuid.encode(),
                b"@revision",
                2,
                b"@timestamp",
                update_timestamp,
                b"@version",
                2,
                b"load",
                b"",
            ],
        )
        assert client.hget(registration, "load") == b""

        retried = expect_status(
            script.call(
                "update",
                uuid,
                2,
                2,
                "load",
                b"",
            ),
            b"ok",
        )
        assert retried[b"@timestamp"] == update_timestamp
        expect_no_event(pubsub)

        transition = expect_status(
            script.call(
                "update",
                uuid,
                4,
                "",
                "load",
                b"2",
            ),
            b"error",
            b"transition",
        )
        assert transition[b"@revision"] == 2

        stale = expect_status(
            script.call(
                "update",
                uuid,
                1,
                "",
                "load",
                b"0",
            ),
            b"error",
            b"stale",
        )
        assert stale[b"@revision"] == 2

        future_renew = expect_status(
            script.call(
                "renew",
                uuid,
                3,
            ),
            b"error",
            b"transition",
        )
        assert future_renew[b"@revision"] == 2

        renewed = expect_status(
            script.call(
                "renew",
                uuid,
                2,
            ),
            b"ok",
        )
        renew_timestamp = renewed[b"@timestamp"]
        assert renew_timestamp >= update_timestamp
        assert_event(
            next_event(pubsub),
            [
                b"&protocol",
                b"v1",
                b"&kind",
                b"renew",
                b"@uuid",
                uuid.encode(),
                b"@revision",
                2,
                b"@timestamp",
                renew_timestamp,
            ],
        )

        client.hdel(script.registry, uuid)
        membership_repair = expect_status(
            script.call(
                "renew",
                uuid,
                2,
            ),
            b"ok",
        )
        renew_timestamp = membership_repair[b"@timestamp"]
        assert client.hget(script.registry, uuid) == b"2"
        assert_event(
            next_event(pubsub),
            [
                b"&protocol",
                b"v1",
                b"&kind",
                b"renew",
                b"@uuid",
                uuid.encode(),
                b"@revision",
                2,
                b"@timestamp",
                renew_timestamp,
            ],
        )

        repaired = expect_status(
            script.register(
                uuid,
                2,
                version=2,
                load=b"",
            ),
            b"ok",
        )
        assert repaired[b"@revision"] == 2
        next_event(pubsub)
        assert client.hget(script.registry, uuid) == b"2"

        reset = expect_status(
            script.register(
                uuid,
                5,
                version=3,
                address=b"10.0.0.2:8080",
                load=None,
            ),
            b"ok",
        )
        assert reset[b"@revision"] == 5
        next_event(pubsub)
        assert client.hget(registration, "load") is None
        assert client.hget(script.registry, uuid) == b"5"

        stale_register = expect_status(
            script.register(uuid, 4, version=3, address=b"10.0.0.9:8080"),
            b"error",
            b"stale",
        )
        assert stale_register[b"@revision"] == 5
        expect_no_event(pubsub)

        expect_status(script.call("unregister", uuid), b"ok")
        assert_event(
            next_event(pubsub),
            [
                b"&protocol",
                b"v1",
                b"&kind",
                b"unregister",
                b"@uuid",
                uuid.encode(),
            ],
        )
        assert client.exists(registration) == 0
        assert client.hexists(script.registry, uuid) == 0

        expect_status(script.call("unregister", uuid), b"ok")
        expect_no_event(pubsub)
    finally:
        pubsub.close()
        client.delete(registration, script.registry)


def exercise_expiry(client: redis.Redis, script: RegistrationScripts) -> None:
    uuid = "fedcba9876543210fedcba9876543210"
    registration = script.key(uuid)
    pubsub = client.pubsub()
    pubsub.subscribe(script.registry)
    wait_for_subscription(pubsub)
    try:
        expect_status(script.register(uuid, 1, ttl=150), b"ok")
        next_event(pubsub)
        wait_until_absent(client, registration)
        assert client.hexists(script.registry, uuid) == 0
        expect_no_event(pubsub)
    finally:
        pubsub.close()
        client.delete(registration, script.registry)


def exercise_sdk_validation_boundary(client: redis.Redis, script: RegistrationScripts) -> None:
    uuid = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    registration = script.key(uuid)
    pubsub = client.pubsub()
    pubsub.subscribe(script.registry)
    wait_for_subscription(pubsub)

    try:
        missing = expect_status(
            script.call(
                "update",
                uuid,
                1,
                "",
                "load",
                b"1",
            ),
            b"error",
            b"missing",
        )
        assert b"@revision" not in missing

        overflow = expect_status(
            script.register(
                uuid,
                1,
                ttl=MAX_SAFE_INTEGER,
            ),
            b"error",
            b"invalid",
        )
        assert overflow[b"&field"] == b"@ttl"
        assert client.exists(registration) == 0
        assert client.hexists(script.registry, uuid) == 0
        expect_no_event(pubsub)

        # Direct script access is outside the SDK/ACL contract. Prove Lua does
        # not duplicate field-count or field-size validation: the selected
        # Register SHA treats these as prevalidated SDK output.
        sdk_bypass = [
            uuid,
            1,
            TTL_MS,
            1,
            "oversized",
            b"x" * (16 * 1024 + 1),
        ]
        for index in range(129):
            sdk_bypass.extend((f"field_{index:03d}", b"x"))
        expect_status(script.call_script("register", uuid, *sdk_bypass), b"ok")
        bypass_event = decoded_pairs(next_event(pubsub))
        assert bypass_event[b"&protocol"] == b"v1"
        assert bypass_event[b"&kind"] == b"register"
        assert bypass_event[b"oversized"] == b"x" * (16 * 1024 + 1)
        assert client.hlen(registration) == 5 + 130

        maximum = expect_status(
            script.register(
                uuid,
                MAX_SAFE_INTEGER,
                version=MAX_SAFE_INTEGER,
                address=b"\x00\xff\n",
            ),
            b"ok",
        )
        assert maximum[b"@revision"] == MAX_SAFE_INTEGER
        maximum_event = decoded_pairs(next_event(pubsub))
        assert maximum_event[b"@revision"] == MAX_SAFE_INTEGER
        assert maximum_event[b"@version"] == MAX_SAFE_INTEGER
        assert maximum_event[b"address"] == b"\x00\xff\n"
        assert client.hmget(registration, "@revision", "@version") == [
            str(MAX_SAFE_INTEGER).encode(),
            str(MAX_SAFE_INTEGER).encode(),
        ]
        assert client.hget(script.registry, uuid) == str(MAX_SAFE_INTEGER).encode()
    finally:
        pubsub.close()
        client.delete(registration, script.registry)


def exercise_update_without_full_record_scan(
    client: redis.Redis,
    script: RegistrationScripts,
) -> None:
    uuid = "cccccccccccccccccccccccccccccccc"
    registration = script.key(uuid)
    try:
        register = [
            1,
            TTL_MS,
            1,
            "d000",
            b"0",
        ]
        for index in range(1, 128):
            register.extend((f"d{index:03d}", b"x" * 494))
        expect_status(script.call("register", uuid, *register), b"ok")
        assert client.hlen(registration) == 133

        # Update must remain proportional to its patch. Reset the isolated
        # fixture's command counters after creating a protocol-ceiling field
        # map so Redis can prove that Lua issued no HGETALL while installing a
        # new content revision.
        client.execute_command("CONFIG", "RESETSTAT")
        expect_status(
            script.call(
                "update",
                uuid,
                2,
                "",
                "d000",
                b"1",
            ),
            b"ok",
        )

        command_stats = client.info("commandstats")
        hgetall = command_stats.get("cmdstat_hgetall", {})
        assert int(hgetall.get("calls", 0)) == 0, hgetall
    finally:
        client.delete(registration, script.registry)


def exercise_script_reload(client: redis.Redis, script: RegistrationScripts) -> None:
    uuid = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
    registration = script.key(uuid)
    try:
        expect_status(script.register(uuid, 1), b"ok")

        client.script_flush()
        expect_status(script.register(uuid, 1), b"ok")

        client.script_flush()
        expect_status(
            script.call(
                "update",
                uuid,
                2,
                "",
                "load",
                b"1",
            ),
            b"ok",
        )

        client.script_flush()
        expect_status(script.call("renew", uuid, 2), b"ok")

        client.script_flush()
        expect_status(script.call("unregister", uuid), b"ok")
        assert client.exists(registration) == 0
    finally:
        client.delete(registration, script.registry)


def exercise_selector_bootstrap(client: redis.Redis, script: RegistrationScripts) -> None:
    uuids = [
        "11111111111111111111111111111111",
        "22222222222222222222222222222222",
        "33333333333333333333333333333333",
    ]
    pubsub = client.pubsub()
    pubsub.subscribe(script.registry)
    wait_for_subscription(pubsub)

    try:
        for revision, uuid in enumerate(uuids, start=1):
            expect_status(script.register(uuid, revision), b"ok")

        membership: dict[bytes, bytes] = {}
        cursor = 0
        while True:
            cursor, page = client.hscan(script.registry, cursor=cursor, count=1)
            membership.update(page)
            if cursor == 0:
                break
        assert membership == {uuid.encode(): str(revision).encode() for revision, uuid in enumerate(uuids, start=1)}

        pipeline = client.pipeline(transaction=False)
        for uuid in uuids:
            pipeline.hmget(script.key(uuid), "@revision", "@timestamp")
        headers = pipeline.execute()
        assert [header[0] for header in headers] == [b"1", b"2", b"3"]

        pipeline = client.pipeline(transaction=False)
        for uuid in uuids:
            pipeline.hgetall(script.key(uuid))
        records = pipeline.execute()
        assert [record[b"@uuid"] for record in records] == [uuid.encode() for uuid in uuids]

        nonce = secrets.token_hex(8)
        pubsub.ping(nonce)
        events: dict[bytes, list[Any]] = {}
        deadline = time.monotonic() + 2.0
        pong = False
        while time.monotonic() < deadline and not pong:
            message = pubsub.get_message(timeout=0.1)
            if not message:
                continue
            if message["type"] == "message":
                event = msgpack.unpackb(message["data"], raw=True)
                event_fields = decoded_pairs(event)
                events[event_fields[b"@uuid"]] = event
            elif message["type"] == "pong" and message["data"] == nonce.encode():
                pong = True

        assert pong, "missing post-scan PONG barrier"
        assert set(events) == {uuid.encode() for uuid in uuids}
    finally:
        pubsub.close()
        client.delete(*(script.key(uuid) for uuid in uuids), script.registry)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--redis-url",
        default=os.environ.get("VERDANDI_REDIS_URL", "redis://127.0.0.1:6379/0"),
    )
    arguments = parser.parse_args()

    generated = generate_registration.expected_outputs(generate_registration.load_manifest())
    if generate_registration.check(generated) != 0:
        raise RuntimeError("Registration Lua generated files are stale")

    client = redis.Redis.from_url(arguments.redis_url, decode_responses=False)
    version = tuple(int(part) for part in client.info("server")["redis_version"].split(".")[:2])
    if version < (8, 0):
        raise RuntimeError(f"Redis 8.0 or later is required, found {version!r}")

    zone = "LuaTest" + "".join(secrets.choice(string.ascii_letters) for _ in range(12))
    script = RegistrationScripts(client, zone, "proxy")

    exercise_registration(client, script)
    exercise_expiry(client, script)
    exercise_sdk_validation_boundary(client, script)
    exercise_update_without_full_record_scan(client, script)
    exercise_selector_bootstrap(client, script)
    exercise_script_reload(client, script)
    redis_version = client.info("server")["redis_version"]
    print(f"PASS Registration atomic Lua glue and Selector bootstrap on Redis {redis_version}")


if __name__ == "__main__":
    main()
