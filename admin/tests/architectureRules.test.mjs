// 架构规则回归: 拒绝上层耦合、提前加载和真实运行时循环依赖.
import assert from "node:assert/strict";
import test from "node:test";
import { allowedDependency, findImportCycles } from "../scripts/architecture-rules.mjs";

const prefix = "features/galaxy/";
const dependency = { value: "./module.ts", dynamic: false, typeOnly: false };

test("the composable owns the only runtime entry and must load it dynamically", () => {
  assert.equal(allowedDependency(prefix + "composables/useGalaxyScene.ts", prefix + "runtime/createGalaxyScene.ts", dependency), false);
  assert.equal(allowedDependency(prefix + "composables/useGalaxyScene.ts", prefix + "runtime/createGalaxyScene.ts", { ...dependency, dynamic: true }), true);
  assert.equal(allowedDependency(prefix + "ui/GalaxyView.vue", prefix + "runtime/createGalaxyScene.ts", { ...dependency, dynamic: true }), false);
  assert.equal(allowedDependency("app/App.vue", prefix + "runtime/createGalaxyScene.ts", dependency), false);
});

test("black-hole mathematics and shaders cannot acquire GPU, Vue or scene dependencies", () => {
  for (const file of ["config.ts", "flow.ts", "optics.ts", "shaders/horizon.ts"]) {
    const from = prefix + "runtime/blackHole/" + file;
    for (const value of ["vue", "three", "three/addons/loaders/GLTFLoader.js"]) {
      assert.equal(allowedDependency(from, null, { ...dependency, value }), false);
    }
    assert.equal(allowedDependency(from, prefix + "runtime/createGalaxyScene.ts", dependency), false);
  }
  assert.equal(allowedDependency(prefix + "runtime/blackHole/optics.ts", prefix + "runtime/blackHole/config.ts", dependency), true);
  assert.equal(allowedDependency(prefix + "runtime/blackHole/shaders/plasma.ts", prefix + "runtime/blackHole/flow.ts", dependency), true);
  assert.equal(allowedDependency(prefix + "runtime/blackHole/material.ts", null, { ...dependency, value: "three" }), true);
});

test("object construction can borrow asset types but cannot load assets or assemble the scene", () => {
  const from = prefix + "runtime/objects/createStarSystems.ts";
  assert.equal(allowedDependency(from, prefix + "runtime/celestialAssets.ts", dependency), false);
  assert.equal(allowedDependency(from, prefix + "runtime/celestialAssets.ts", { ...dependency, typeOnly: true }), true);
  assert.equal(allowedDependency(from, prefix + "runtime/createGalaxyScene.ts", dependency), false);
  assert.equal(allowedDependency(from, prefix + "runtime/blackHole/createBlackHole.ts", dependency), true);
  assert.equal(allowedDependency(from, prefix + "data/demo.ts", dependency), false);
});

test("dependency traversal accepts diamonds and reports cycles including self imports", () => {
  const graph = new Map([
    ["a", ["b", "c"]],
    ["b", ["d"]],
    ["c", ["d"]],
    ["d", []],
  ]);
  assert.deepEqual(findImportCycles(graph), []);
  graph.set("d", ["a"]);
  assert.deepEqual(findImportCycles(graph), [["a", "b", "d", "a"]]);
  assert.deepEqual(findImportCycles(new Map([["a", ["a"]]])), [["a", "a"]]);
});
