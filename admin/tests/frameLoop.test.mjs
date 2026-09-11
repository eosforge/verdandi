import assert from "node:assert/strict";
import test from "node:test";
import { FrameLoop } from "../src/features/galaxy/runtime/frameLoop.ts";

function scheduler() {
  let nextId = 0;
  const pending = new Map();
  return {
    request(callback) {
      const id = nextId++;
      pending.set(id, callback);
      return id;
    },
    cancel(id) {
      pending.delete(id);
    },
    fire(time) {
      const callbacks = [...pending.values()];
      pending.clear();
      callbacks.forEach((callback) => callback(time));
    },
    get size() {
      return pending.size;
    },
  };
}

for (const refreshRate of [30, 32, 60, 120, 144]) {
  test(`${refreshRate} Hz: reports actual draws without halving a 60 Hz display`, () => {
    const clock = scheduler();
    const deltas = [];
    const reports = [];
    const loop = new FrameLoop(
      clock,
      (delta) => deltas.push(delta),
      (fps) => reports.push(fps),
      assert.fail,
    );
    loop.start();
    loop.start();
    for (let frame = 0; frame <= refreshRate * 3; frame++) {
      assert.equal(clock.size, 1);
      clock.fire((frame * 1000) / refreshRate);
    }
    const expected = Math.min(60, refreshRate);
    assert.ok(Math.abs(deltas.length - (expected * 3 + 1)) <= 1);
    assert.ok(reports.length >= 2);
    reports.forEach((fps) => assert.ok(Math.abs(fps - expected) <= 1));
    loop.dispose();
    assert.equal(clock.size, 0);
  });
}

test("pause discards hidden time, clamps stalls, and disposal prevents restart", () => {
  const clock = scheduler();
  const deltas = [];
  const loop = new FrameLoop(
    clock,
    (delta) => deltas.push(delta),
    () => {},
    assert.fail,
  );
  loop.start();
  loop.stop();
  assert.equal(clock.size, 0, "request id zero must also be cancelled");
  loop.start();
  clock.fire(0);
  clock.fire(1000);
  assert.equal(deltas.at(-1), 0.1);
  loop.stop();
  clock.fire(10000);
  assert.equal(deltas.length, 2);
  loop.start();
  clock.fire(20000);
  assert.equal(deltas.at(-1), 0);
  loop.dispose();
  loop.dispose();
  loop.start();
  assert.equal(clock.size, 0);
});

test("render failure stops scheduling and reports the original error once", () => {
  const clock = scheduler();
  const failure = new Error("draw failed");
  const errors = [];
  const loop = new FrameLoop(
    clock,
    () => {
      throw failure;
    },
    () => {},
    (error) => errors.push(error),
  );
  loop.start();
  clock.fire(0);
  clock.fire(100);
  assert.deepEqual(errors, [failure]);
  assert.equal(clock.size, 0);
});
