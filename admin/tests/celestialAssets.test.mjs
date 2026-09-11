import assert from "node:assert/strict";
import test from "node:test";
import { PerspectiveCamera, Raycaster, Vector3 } from "three";
import { createBlackHole } from "../src/features/galaxy/runtime/createBlackHole.ts";
import { blackHoleOptics } from "../src/features/galaxy/runtime/materials/blackHoleOptics.ts";
import { loadCelestialModels } from "../src/features/galaxy/runtime/celestialAssets.ts";
import { createGalaxyScene } from "../src/features/galaxy/runtime/createGalaxyScene.ts";
import { demoGalaxy } from "../src/features/galaxy/data/demo.ts";
import { ResourceScope } from "../src/features/galaxy/runtime/resourceScope.ts";
import { loadTestModels, modelFiles, readModelBytes } from "./support/celestialModels.mjs";

test("GLB models contain their own buffers; clones share resources and dispose them exactly once", async () => {
  const scope = new ResourceScope();
  const models = await loadTestModels(scope);
  const resources = new Map();
  for (const model of Object.values(models)) {
    model.traverse((object) => {
      if (!object.isMesh) return;
      const textures = Object.values(object.material.uniforms ?? {})
        .map((uniform) => uniform.value)
        .filter((value) => value?.isTexture);
      for (const resource of [object.geometry, object.material, ...textures]) {
        if (resources.has(resource)) continue;
        resources.set(resource, 0);
        resource.addEventListener("dispose", () => resources.set(resource, resources.get(resource) + 1));
      }
    });
  }
  const first = createBlackHole(models.blackHole);
  const second = createBlackHole(models.blackHole);
  const positions = first.group.getObjectByName("Disc").geometry.getAttribute("position");
  let minimumRadius = Infinity;
  let maximumRadius = 0;
  let halfHeight = 0;
  for (let index = 0; index < positions.count; index++) {
    const radius = Math.hypot(positions.getX(index), positions.getZ(index));
    minimumRadius = Math.min(minimumRadius, radius);
    maximumRadius = Math.max(maximumRadius, radius);
    halfHeight = Math.max(halfHeight, Math.abs(positions.getY(index)));
  }
  assert.ok(Math.abs(minimumRadius - 3 * blackHoleOptics.schwarzschildRadius) < 1e-5, "the authored inner edge follows the Schwarzschild ISCO");
  assert.ok(Math.abs(maximumRadius - blackHoleOptics.discOuter) < 1e-5, "the authored outer edge matches the optical disc");
  assert.ok(Math.abs(halfHeight - blackHoleOptics.discHalfHeight) < 1e-5, "the foreground volume and mesh share their thickness");
  first.group.updateMatrixWorld(true);
  assert.equal(first.core.visible, false, "the shadow must not be rasterized as a protruding sphere");
  const ray = new Raycaster(new Vector3(0, 0, 30), new Vector3(0, 0, -1));
  assert.ok(ray.intersectObject(first.core).length > 0, "the hidden model core remains available for node picking");
  assert.equal(first.core.geometry, second.core.geometry);
  first.update(1);
  assert.deepEqual(
    first.group.getObjectByName("Accretion").quaternion.toArray(),
    second.group.getObjectByName("Accretion").quaternion.toArray(),
    "flow preserves the model pose",
  );
  const opticalAxis = first.group.getObjectByName("Horizon").material.uniforms.discNormal.value;
  const discAxis = new Vector3(0, 1, 0).applyQuaternion(first.group.getObjectByName("Accretion").quaternion);
  assert.ok(opticalAxis.distanceTo(discAxis) < 1e-10, "optical material stays aligned with the authored GLB disc");
  first.group.updateMatrixWorld(true);
  const horizon = first.group.getObjectByName("Horizon");
  const sample = new Vector3(5, 0, 2);
  const direct = first.group.getObjectByName("Accretion").localToWorld(sample.clone());
  const lensed = horizon.localToWorld(sample.clone().applyMatrix3(horizon.material.uniforms.discBasis.value.clone().invert()));
  assert.ok(direct.distanceTo(lensed) < 1e-10, "direct and lensed texture coordinates use the same disc basis");
  assert.equal(horizon.material.uniforms.bending.value, second.group.getObjectByName("Horizon").material.uniforms.bending.value);
  for (const name of Object.values(modelFiles)) {
    const bytes = await readModelBytes(name);
    const header = new DataView(bytes);
    assert.equal(header.getUint32(0, true), 0x46546c67);
    const json = JSON.parse(new TextDecoder().decode(bytes.slice(20, 20 + header.getUint32(12, true))));
    assert.ok(json.buffers.every((buffer) => !buffer.uri));
    assert.equal(json.images, undefined);
  }
  assert.deepEqual(scope.dispose(), []);
  assert.deepEqual(scope.dispose(), []);
  assert.ok([...resources.values()].every((count) => count === 1));
});

test("black-hole draws bind independent clocks and distance detail without cloning shared resources", async () => {
  const scope = new ResourceScope();
  try {
    const models = await loadTestModels(scope);
    const first = createBlackHole(models.blackHole);
    const second = createBlackHole(models.blackHole);
    first.group.updateMatrixWorld(true);
    second.group.updateMatrixWorld(true);
    const firstVolume = first.group.getObjectByName("Horizon");
    const secondVolume = second.group.getObjectByName("Horizon");
    const material = firstVolume.material;
    assert.equal(material, secondVolume.material);
    const camera = new PerspectiveCamera(45, 16 / 9, 0.1, 1000);
    camera.position.z = 48;
    camera.updateMatrixWorld(true);
    const renderer = { getDrawingBufferSize: (target) => target.set(1280, 720) };
    first.update(1);
    firstVolume.onBeforeRender(renderer, null, camera);
    assert.equal(material.uniforms.flowTime.value, 3, "one elapsed second advances the entire flow by three virtual seconds");
    assert.deepEqual(material.uniforms.localEye.value.toArray(), [0, 0, 48]);
    const nearDetail = material.uniforms.detailLevel.value;
    assert.ok(nearDetail > 0);
    assert.ok(material.uniforms.brightKnots.value.every((knot) => [...knot].every(Number.isFinite)));
    material.uniformsNeedUpdate = false;
    secondVolume.onBeforeRender(renderer, null, camera);
    assert.equal(material.uniforms.flowTime.value, 0);
    assert.deepEqual(material.uniforms.localEye.value.toArray(), [0, 0, 48]);
    assert.equal(material.uniformsNeedUpdate, true, "consecutive draws of one material must upload the new instance clock");
    for (let index = 0; index < 120; index++) second.update(1 / 120);
    secondVolume.onBeforeRender(renderer, null, camera);
    assert.ok(Math.abs(material.uniforms.flowTime.value - 3) < 1e-12);
    first.group.position.set(12, -5, 3);
    first.group.rotation.y = 0.3;
    first.group.scale.setScalar(2);
    first.group.updateMatrixWorld(true);
    firstVolume.onBeforeRender(renderer, null, camera);
    const observerWorld = firstVolume.localToWorld(material.uniforms.localEye.value.clone());
    assert.ok(observerWorld.distanceTo(camera.position) < 1e-10, "the observer transforms back to the camera for translated, rotated and scaled instances");
    secondVolume.onBeforeRender(renderer, null, camera);
    assert.deepEqual(material.uniforms.localEye.value.toArray(), [0, 0, 48], "each shared-material draw binds its own local observer");
    first.update(47.01);
    first.update(Number.NaN);
    first.update(-5);
    firstVolume.onBeforeRender(renderer, null, camera);
    assert.ok(Math.abs(material.uniforms.flowTime.value - 0.03) < 1e-12, "only the bounded shader phase wraps; invalid deltas are ignored");
    secondVolume.onBeforeRender(renderer, null, camera);
    assert.ok(Math.abs(material.uniforms.flowTime.value - 3) < 1e-12);
    camera.position.z = 1000;
    camera.updateMatrixWorld(true);
    firstVolume.onBeforeRender(renderer, null, camera);
    assert.equal(material.uniforms.detailLevel.value, 0);
    assert.ok(material.uniforms.detailLevel.value < nearDetail);
    const farKnots = material.uniforms.brightKnots.value.map((knot) => knot.clone());
    assert.ok(
      farKnots.some((knot) => knot.z > 0),
      "resolved distant nodes retain broad moving highlights",
    );
    first.update(1);
    firstVolume.onBeforeRender(renderer, null, camera);
    assert.ok(
      material.uniforms.brightKnots.value.some((knot, index) => knot.x !== farKnots[index].x),
      "distant highlights keep advancing",
    );
  } finally {
    assert.deepEqual(scope.dispose(), []);
  }
});

test("model loading errors settle all reads and an already-cancelled scene never fetches or creates a canvas", async () => {
  const originalFetch = globalThis.fetch;
  const scope = new ResourceScope();
  const requests = [];
  globalThis.fetch = async (url) => {
    requests.push(url.pathname);
    return new Response(null, { status: 503 });
  };
  try {
    await assert.rejects(loadCelestialModels(demoGalaxy, scope), /Cannot load .* model: 503/);
    assert.equal(requests.length, 3);
    const cancellation = new AbortController();
    cancellation.abort();
    await assert.rejects(createGalaxyScene(null, demoGalaxy, {}, cancellation.signal), { name: "AbortError" });
    assert.equal(requests.length, 3);
    assert.deepEqual(await loadCelestialModels({ ...demoGalaxy, peers: [], links: [] }, scope), {});
    assert.equal(requests.length, 3);
    globalThis.fetch = (url, { signal }) => {
      requests.push(url.pathname);
      return new Promise((_resolve, reject) => signal.addEventListener("abort", () => reject(signal.reason), { once: true }));
    };
    const inFlight = new AbortController();
    const pending = createGalaxyScene(null, demoGalaxy, {}, inFlight.signal);
    assert.equal(requests.length, 6);
    inFlight.abort();
    await assert.rejects(pending, { name: "AbortError" });
  } finally {
    globalThis.fetch = originalFetch;
    assert.deepEqual(scope.dispose(), []);
  }
});
