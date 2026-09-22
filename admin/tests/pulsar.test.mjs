// 脉冲星三维结构、磁轴姿态与资源生命周期; 不宣称覆盖 GPU 成像.
import assert from "node:assert/strict";
import test from "node:test";
import { Vector3 } from "three";
import { createPulsar } from "../src/features/galaxy/runtime/pulsar/createPulsar.ts";
import { pulsarConfig } from "../src/features/galaxy/runtime/pulsar/config.ts";
import { ResourceScope } from "../src/features/galaxy/runtime/resourceScope.ts";
import { loadCelestialModels } from "../src/features/galaxy/runtime/celestialAssets.ts";
import { demoGalaxy } from "../src/features/galaxy/data/demo.ts";

test("opposite magnetic beams sweep around a separate spin axis and share the core's pose", () => {
  const scope = new ResourceScope();
  try {
    const pulsar = createPulsar(scope);
    pulsar.group.updateMatrixWorld(true);
    const north = pulsar.group.getObjectByName("PulsarNorthBeam");
    const south = pulsar.group.getObjectByName("PulsarSouthBeam");
    const axis = (node) => new Vector3(0, 1, 0).transformDirection(node.matrixWorld);
    const first = axis(north);
    const spin = axis(pulsar.rotor);
    assert.ok(Math.abs(first.angleTo(spin) - pulsarConfig.magneticTilt) < 1e-10);
    assert.ok(first.clone().add(axis(south)).length() < 1e-10);
    assert.equal(north.geometry, south.geometry);
    assert.notEqual(north.material.uniforms.localCamera.value, south.material.uniforms.localCamera.value);
    assert.equal(pulsar.core.geometry.type, "SphereGeometry");
    pulsar.rotor.rotateY(Math.PI);
    pulsar.group.updateMatrixWorld(true);
    assert.ok(Math.abs(first.angleTo(axis(north)) - 2 * pulsarConfig.magneticTilt) < 1e-10);
    assert.ok(axis(north).add(axis(south)).length() < 1e-10);
    assert.ok(new Vector3().setFromMatrixPosition(pulsar.core.matrixWorld).length() < 1e-10);
  } finally {
    assert.deepEqual(scope.dispose(), []);
  }
});

test("all generated pulsar geometry and materials are disposed exactly once", () => {
  const scope = new ResourceScope();
  const pulsar = createPulsar(scope);
  const resources = new Map();
  pulsar.group.traverse((object) => {
    if (!object.isMesh) return;
    for (const resource of [object.geometry, object.material]) {
      if (resources.has(resource)) continue;
      resources.set(resource, 0);
      resource.addEventListener("dispose", () => resources.set(resource, resources.get(resource) + 1));
    }
  });
  assert.ok(resources.size > 0);
  assert.deepEqual(scope.dispose(), []);
  assert.deepEqual(scope.dispose(), []);
  assert.ok([...resources.values()].every((count) => count === 1));
});

test("flow phase is instance-local, shared by its emitters and continuous under period wrapping", () => {
  const scope = new ResourceScope();
  try {
    const first = createPulsar(scope);
    const second = createPulsar(scope);
    const phase = first.core.material.uniforms.phase;
    for (const name of ["PulsarNorthBeam", "PulsarSouthBeam", "PulsarField", "PulsarFieldGlow"])
      assert.equal(first.group.getObjectByName(name).material.uniforms.phase, phase);
    first.update(pulsarConfig.flowSeconds / 4);
    assert.ok(Math.abs(phase.value - Math.PI / 2) < 1e-12);
    assert.equal(second.core.material.uniforms.phase.value, 0);
    first.update(pulsarConfig.flowSeconds * 20);
    assert.ok(Math.abs(phase.value - Math.PI / 2) < 1e-12);
    first.update((pulsarConfig.flowSeconds * 3) / 4);
    assert.ok(Math.abs(phase.value) < 1e-12);
    for (const delta of [NaN, Infinity, -1, 0]) first.update(delta);
    assert.equal(phase.value, 0);
    assert.deepEqual(first.rotor.quaternion.toArray(), second.rotor.quaternion.toArray(), "flow must not take ownership of spin");
    const field = first.group.getObjectByName("PulsarField").geometry;
    assert.equal(new Set(field.getAttribute("fieldSeed").array).size, 24, "three differently sized field bands survive merging");
  } finally {
    assert.deepEqual(scope.dispose(), []);
  }
});

test("a standalone procedural pulsar loads no GLB or external asset", async () => {
  const scope = new ResourceScope();
  const originalFetch = globalThis.fetch;
  let requests = 0;
  globalThis.fetch = async () => {
    requests++;
    throw new Error("Unexpected asset request");
  };
  try {
    const data = { ...demoGalaxy, stars: demoGalaxy.stars.filter((star) => star.appearance === "pulsar"), links: [] };
    assert.equal(data.stars.length, 1);
    assert.deepEqual(await loadCelestialModels(data, scope), {});
    assert.equal(requests, 0);
  } finally {
    globalThis.fetch = originalFetch;
    scope.dispose();
  }
});
