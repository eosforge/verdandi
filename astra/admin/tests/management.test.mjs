import assert from "node:assert/strict";
import test from "node:test";
import { Api, Failure, decimal, observation, samples, scopes, successor } from "../src/app/management/api.ts";

const signal = () => new AbortController().signal;
const snapshot = (rows, version = "1") =>
  rows.map((row) => JSON.stringify(row)).join("\n") +
  (rows.length ? "\n" : "") +
  JSON.stringify({ complete: true, position: { sector: "a", spectrum: "b", version } }) +
  "\n";
const api = (body) => new Api("https://admin.example", async () => new Response(body));

test("browser fetch retains its required global receiver", async () => {
  const client = new Api("https://admin.example", async function () {
    assert.equal(this, globalThis);
    return new Response('{"ready":true}');
  });
  assert.deepEqual(await client.json("session", signal()), { ready: true });
});

test("management versions and authority inventory retain uint64 precision", () => {
  assert.equal(decimal("18446744073709551615"), "18446744073709551615");
  assert.equal(successor("18446744073709551615"), null);
  assert.equal(successor("18446744073709551614"), "18446744073709551615");
  assert.equal(successor("0"), "1");
  for (const value of [1, "01", "-1", "18446744073709551616"]) assert.throws(() => decimal(value));
  assert.deepEqual(scopes({ complete: true, positions: [{ sector: "a", spectrum: "b", version: "1" }] }), [{ sector: "a", spectrum: "b", version: "1" }]);
  assert.throws(() => scopes({ complete: false, positions: [] }));
});

test("malformed responses retain HTTP status without reflecting response secrets", async () => {
  const client = new Api("https://admin.example", async () => new Response("private-secret", { status: 401 }));
  await assert.rejects(
    client.json("session", signal()),
    (error) => error instanceof Failure && error.status === 401 && error.effect === "unknown" && !error.message.includes("private-secret"),
  );
  await assert.rejects(api("private-secret").json("almanac", signal()), (error) => error.message === "管理响应不是有效 JSON");
  await assert.rejects(api("private-secret\n").load("a", "b", signal()), (error) => error.message === "管理响应不是有效 JSON");
});

test("snapshot installs only complete matching scope and distinguishes empty/redacted data", async () => {
  const view = await api(
    snapshot([
      { key: "empty", value: "" },
      { key: "secret", redacted: true },
    ]),
  ).load("a", "b", signal());
  assert.deepEqual(view, {
    version: "1",
    records: [
      { key: "empty", value: "" },
      { key: "secret", value: null },
    ],
  });
  assert.deepEqual(await api(snapshot([], "0")).load("a", "b", signal()), { version: "0", records: [] });
  for (const body of [
    "",
    '{"key":"x","value":""}\n',
    snapshot([
      { key: "x", value: "" },
      { key: "x", value: "" },
    ]),
    snapshot([]) + '{"key":"late","value":""}\n',
    snapshot([]).replace('"a"', '"other"'),
  ]) {
    await assert.rejects(api(body).load("a", "b", signal()));
  }
});

test("NDJSON decoding handles arbitrary UTF-8 transport boundaries", async () => {
  const encoded = new TextEncoder().encode(snapshot([{ key: "中文", value: "" }]));
  const stream = new ReadableStream({
    start(controller) {
      for (const byte of encoded) controller.enqueue(Uint8Array.of(byte));
      controller.close();
    },
  });
  const view = await api(stream).load("a", "b", signal());
  assert.equal(view.records[0].key, "中文");
});

test("failed writes are sent once and never silently replayed", async () => {
  let calls = 0;
  const client = new Api("https://admin.example", async (_url, options) => {
    calls++;
    assert.equal(options.credentials, "include");
    assert.equal(options.redirect, "error");
    throw new TypeError("network gone after send");
  });
  await assert.rejects(client.json("almanac", signal(), "POST", { value: "" }));
  assert.equal(calls, 1);
  const rejected = new Api("https://admin.example", async () => new Response('{"effect":"unapplied"}', { status: 409 }));
  await assert.rejects(rejected.json("almanac", signal(), "POST", {}), (error) => error instanceof Failure && error.effect === "unapplied");
});

test("observations retain stale/unknown and do not invent live topology", () => {
  const node = { id: "one", role: "ROLE_STAR", galaxy: "test", group: "default", endpoint: "127.0.0.1:1", epoch: "1" };
  assert.equal(observation({ nodes: [node], observed: "2026-09-21T00:00:00Z", stale: true }).stale, true);
  assert.throws(() => observation({ nodes: [node, node], observed: "", stale: false }));
  assert.deepEqual(samples({ samples: [{ id: "one", observed: "", stale: true, values: null }] })[0].values, {});
  for (const origin of ["https://user:pass@admin.example", "https://admin.example/path", "file:///tmp/admin"]) assert.throws(() => new Api(origin));
});

// 指标与目录使用相同的节点容量, 不能在后端已采集后因超过 64 项整批拒绝展示.
test("metrics cover the full directory and reject ambiguous identities", () => {
  const entries = Array.from({ length: 128 }, (_, index) => ({ id: `star-${index}`, observed: "", stale: true, values: null }));
  const result = samples({ samples: entries });
  assert.equal(result.length, entries.length);
  assert.equal(result.at(-1).id, "star-127");
  assert.ok(result.every((sample) => sample.stale && Object.keys(sample.values).length === 0));
  assert.throws(() => samples({ samples: [entries[0], entries[0]] }));
  assert.throws(() => samples({ samples: [{ ...entries[0], id: "" }] }));
});
