import assert from "node:assert/strict";
import test from "node:test";
import { ResourceScope } from "../src/features/galaxy/runtime/resourceScope.ts";

test("resource cleanup is reversed, idempotent, and continues through failures", () => {
  const scope = new ResourceScope();
  const released = [];
  const failure = new Error("dispose failed");
  scope.defer(() => released.push("renderer"));
  const resource = {
    dispose() {
      released.push("geometry");
      throw failure;
    },
  };
  assert.equal(scope.own(resource), resource);
  scope.defer(() => released.push("listeners"));
  assert.deepEqual(scope.dispose(), [failure]);
  assert.deepEqual(released, ["listeners", "geometry", "renderer"]);
  assert.deepEqual(scope.dispose(), []);
  scope.defer(() => released.push("late completion"));
  assert.deepEqual(released, ["listeners", "geometry", "renderer", "late completion"]);
});
