import assert from "node:assert/strict";
import { after, before, test } from "node:test";
import { Raycaster, Scene, Vector3, Quaternion } from "three";
import { demoGalaxy } from "../src/features/galaxy/data/demo.ts";
import { createGalaxyObjects } from "../src/features/galaxy/runtime/createGalaxyObjects.ts";
import { writeOrbitPosition } from "../src/features/galaxy/model/orbit.ts";
import { ResourceScope } from "../src/features/galaxy/runtime/resourceScope.ts";
import { loadTestModels } from "./support/celestialModels.mjs";

// Only the CPU-side image buffer is stubbed. These tests do not claim GPU shader coverage.
const originalDocument = globalThis.document;
before(() => {
  globalThis.document = {
    createElement() {
      return {
        getContext() {
          return {
            createImageData(width, height) {
              return { data: new Uint8ClampedArray(width * height * 4) };
            },
            putImageData() {},
          };
        },
      };
    },
  };
});
after(() => {
  if (originalDocument === undefined) delete globalThis.document;
  else globalThis.document = originalDocument;
});

test("planet counts scale the stellar surface and corona together while preserving star, planet and picking coordinates", async () => {
  const scope = new ResourceScope();
  try {
    const snapshot = structuredClone(demoGalaxy);
    const counts = [10, 30, 180];
    const scales = [0.5, 1, 3];
    snapshot.stars.slice(0, 3).forEach((star, index) => {
      const originals = star.planets;
      star.planets = Array.from({ length: counts[index] }, (_, index) => ({ ...originals[index % originals.length], id: `${star.id}/scale/${index}` }));
    });
    const before = structuredClone(snapshot);
    const objects = createGalaxyObjects(new Scene(), snapshot, scope, await loadTestModels(scope));
    snapshot.stars.slice(0, 3).forEach((star, index) => {
      const system = objects.systemsById.get(star.id);
      const expected = new Vector3().setScalar(scales[index]);
      assert.ok(system.surface.getWorldScale(new Vector3()).distanceTo(expected) < 1e-12);
      assert.ok(system.surface.parent.getObjectByName("Corona").getWorldScale(new Vector3()).distanceTo(expected) < 1e-12);
      assert.deepEqual(system.group.scale.toArray(), [1, 1, 1], "the system frame must not scale with its star");
      assert.deepEqual(system.group.position.toArray(), star.position);
      const planet = star.planets[0];
      const planetPosition = objects.worldPosition(objects.findPlanet(star.id, planet.id));
      const initial = [0, 0, 0];
      writeOrbitPosition(system.shells[0].orbits[0], 0, initial);
      assert.ok(
        planetPosition.distanceTo(new Vector3(...star.position).add(new Vector3(...initial))) < 1e-5,
        "world positions use the planned orbit without scaling the system frame",
      );
      const center = new Vector3(...star.position);
      const radius = 4.5 * scales[index];
      const inside = new Raycaster(center.clone().add(new Vector3(radius * 0.8, 0, 40)), new Vector3(0, 0, -1), 0, 80);
      const outside = new Raycaster(center.clone().add(new Vector3(radius * 1.2, 0, 40)), new Vector3(0, 0, -1), 0, 80);
      assert.equal(objects.pick(inside, null)?.star.id, star.id, "picking reaches the scaled surface");
      assert.equal(objects.pick(outside, null), null, "picking does not retain the unscaled radius");
    });
    const blackHole = objects.systemsById.get("star-orion").surface;
    assert.deepEqual(blackHole.getWorldScale(new Vector3()).toArray(), [0.75, 0.75, 0.75]);
    assert.deepEqual(snapshot, before);
  } finally {
    assert.deepEqual(scope.dispose(), []);
  }
});

test("stellar rotation accepts independent speeds, preserves paused planets and is frame-rate independent", async () => {
  const scope = new ResourceScope();
  try {
    const models = await loadTestModels(scope);
    const objects = createGalaxyObjects(new Scene(), demoGalaxy, scope, models);
    const reference = createGalaxyObjects(new Scene(), demoGalaxy, scope, models);
    const star = demoGalaxy.stars[0];
    const selection = { star, planet: star.planets[0] };
    const surface = objects.systemsById.get(star.id).surface;
    const orientation = () => surface.getWorldQuaternion(new Quaternion());
    const original = orientation();
    const planets = objects.systemsById.get(star.id).shells.map((shell) => shell.mesh.instanceMatrix.array.slice());
    objects.update(15, selection);
    const quarterTurn = new Quaternion().setFromAxisAngle(new Vector3(0, 1, 0), Math.PI / 2);
    for (const candidate of demoGalaxy.stars.filter((candidate) => candidate.status === "available")) {
      assert.ok(
        objects.systemsById.get(candidate.id).surface.getWorldQuaternion(new Quaternion()).angleTo(quarterTurn) < 1e-7,
        "all available stars default to one revolution per minute regardless of planet count",
      );
    }
    objects.update(45, selection);
    assert.ok(orientation().angleTo(original) < 1e-7, "one minute completes the default rotation");
    assert.equal(objects.setStarRotationSpeed(star.id, 0.4), true);
    assert.equal(reference.setStarRotationSpeed(star.id, 0.4), true);
    for (const value of [NaN, Infinity, -Infinity]) assert.equal(objects.setStarRotationSpeed(star.id, value), false);
    assert.equal(objects.setStarRotationSpeed("missing", 0.4), false);
    assert.equal(objects.setStarRotationSpeed("star-orion", 0.4), false);
    objects.update(3, selection);
    for (let frame = 0; frame < 180; frame++) reference.update(1 / 60, null);
    const expected = new Quaternion().setFromAxisAngle(new Vector3(0, 1, 0), 1.2);
    assert.ok(orientation().angleTo(expected) < 1e-7, "speed uses radians per real elapsed second");
    assert.ok(orientation().angleTo(reference.systemsById.get(star.id).surface.getWorldQuaternion(new Quaternion())) < 1e-7);
    assert.deepEqual(
      objects.systemsById.get(star.id).shells.map((shell) => shell.mesh.instanceMatrix.array),
      planets,
    );
    assert.ok(
      objects.systemsById
        .get("star-lyra")
        .surface.getWorldQuaternion(new Quaternion())
        .angleTo(reference.systemsById.get("star-lyra").surface.getWorldQuaternion(new Quaternion())) < 1e-7,
      "other stars retain their default speed",
    );
    assert.deepEqual(
      objects.systemsById.get("star-orion").surface.getWorldQuaternion(new Quaternion()).toArray(),
      original.toArray(),
      "black-hole model pose is unaffected",
    );
    const stopped = orientation();
    objects.setStarRotationSpeed(star.id, 0);
    objects.update(2, selection);
    assert.deepEqual(orientation().toArray(), stopped.toArray(), "zero speed stops at the current orientation");
    objects.setStarRotationSpeed(star.id, -0.4);
    for (const delta of [NaN, Infinity, -1, 0]) objects.update(delta, selection);
    assert.deepEqual(orientation().toArray(), stopped.toArray(), "invalid deltas do not rotate the star");
    objects.update(3, selection);
    assert.ok(orientation().angleTo(original) < 1e-7, "negative speed reverses smoothly");
    objects.setStarRotationSpeed(star.id, Number.MAX_VALUE);
    objects.update(10, selection);
    assert.ok(orientation().toArray().every(Number.isFinite));
    assert.deepEqual(models.star.quaternion.toArray(), original.toArray(), "the shared source model is never rotated");
  } finally {
    assert.deepEqual(scope.dispose(), []);
  }
});

test("elliptic motion retains nine shared GLB batches and uploads each matrix buffer once per frame", async () => {
  const scope = new ResourceScope();
  try {
    const scene = new Scene();
    const models = await loadTestModels(scope);
    const objects = createGalaxyObjects(scene, demoGalaxy, scope, models);
    const meshes = [];
    scene.traverse((object) => {
      if (object.isInstancedMesh) meshes.push(object);
    });
    assert.equal(meshes.length, 9);
    assert.equal(
      meshes.reduce((sum, mesh) => sum + mesh.count, 0),
      144,
    );
    assert.equal(new Set(meshes.map((mesh) => mesh.geometry)).size, 1);
    assert.equal(meshes[0].geometry, models.planet.getObjectByName("Surface").geometry);
    for (const star of demoGalaxy.stars.filter((star) => star.status === "available"))
      assert.equal(objects.systemsById.get(star.id).surface.geometry, models.star.getObjectByName("Surface").geometry);
    assert.equal(new Set(meshes.map((mesh) => mesh.material)).size, 1);
    const versions = meshes.map((mesh) => mesh.instanceMatrix.version);
    objects.update(0.1, null);
    assert.deepEqual(
      meshes.map((mesh) => mesh.instanceMatrix.version),
      versions.map((version) => version + 1),
    );
  } finally {
    assert.deepEqual(scope.dispose(), []);
  }
});

test("unavailable star only exposes its black-hole core, even when a snapshot retains old entities and links", async () => {
  const scope = new ResourceScope();
  try {
    const scene = new Scene();
    const snapshot = structuredClone(demoGalaxy);
    const star = snapshot.stars.find((star) => star.status === "unavailable");
    const planet = { ...snapshot.stars[0].planets[0], id: "orion/stale", starId: star.id };
    star.planets.push(planet);
    snapshot.links.push({ source: "star-atlas", target: star.id }, { source: star.id, target: "star-vega" });
    const before = structuredClone(snapshot);
    const objects = createGalaxyObjects(scene, snapshot, scope, await loadTestModels(scope));
    assert.equal(objects.findPlanet(star.id, planet.id), undefined);
    assert.equal(objects.systemsById.get(star.id).shells.length, 0);
    objects.showSelection({ star, planet });
    assert.equal(objects.selectionRing.visible, false);
    const center = new Vector3(...star.position);
    const starRay = new Raycaster(center.clone().add(new Vector3(0, 0, 6)), new Vector3(0, 0, -1), 0, 3);
    assert.deepEqual(objects.pick(starRay, null), { star, planet: null }, "black-hole core retains the star selection contract");
    const lines = scene.children.filter((object) => object.isLineSegments);
    assert.equal(lines.length, 1);
    assert.equal(lines[0].geometry.getAttribute("position").count, 6, "only the original three available links remain");
    objects.showSelection(null);
    objects.update(10, null);
    assert.equal(objects.findPlanet(star.id, planet.id), undefined);
    assert.deepEqual(snapshot, before, "visibility rules must not mutate the caller's snapshot");
  } finally {
    assert.deepEqual(scope.dispose(), []);
  }
});

test("planet selection pauses orbital motion and picking requires the owning star view", async () => {
  const scope = new ResourceScope();
  try {
    const scene = new Scene();
    const snapshot = structuredClone(demoGalaxy);
    const objects = createGalaxyObjects(scene, snapshot, scope, await loadTestModels(scope));
    const star = snapshot.stars[0];
    const planet = star.planets[0];
    const location = objects.findPlanet(String(star.id), String(planet.id));
    assert.ok(location);
    assert.equal(objects.findPlanet("wrong-star", planet.id), undefined);
    assert.equal(objects.findPlanet(star.id, "missing"), undefined);
    const before = objects.worldPosition(location);
    objects.update(5, null);
    const rotated = objects.worldPosition(location);
    assert.ok(rotated.distanceTo(before) > 0.1);
    const other = objects.findPlanet(snapshot.stars[1].id, snapshot.stars[1].planets[0].id);
    const otherBefore = objects.worldPosition(other);
    objects.showSelection({ star, planet });
    objects.update(5, { star, planet });
    assert.deepEqual(objects.worldPosition(location), rotated);
    assert.deepEqual(objects.worldPosition(other), otherBefore, "planet details freeze all orbital shells");
    assert.deepEqual(objects.selectionRing.position, rotated);
    assert.equal(objects.selectionRing.visible, true);
    const direction = rotated
      .clone()
      .sub(new Vector3(...star.position))
      .normalize();
    const ray = new Raycaster(rotated.clone().addScaledVector(direction, 3), direction.negate(), 0, 4);
    assert.equal(objects.pick(ray, null), null, "overview cannot select a planet");
    assert.equal(objects.pick(ray, { star: snapshot.stars[1], planet: null }), null, "another star view cannot select this planet");
    assert.equal(objects.pick(ray, { star, planet: null })?.planet?.id, planet.id);
    assert.equal(objects.pick(ray, { star, planet })?.planet?.id, planet.id);
    objects.showSelection(null);
    assert.equal(objects.selectionRing.visible, false);
    objects.update(1, null);
    assert.ok(objects.worldPosition(location).distanceTo(rotated) > 0);
    assert.deepEqual(snapshot, demoGalaxy, "renderer must not mutate the input snapshot");
  } finally {
    assert.deepEqual(scope.dispose(), []);
  }
});

test("overview and star clocks use 5:2 rates; planet selection pauses without uploading or resetting poses", async () => {
  const scope = new ResourceScope();
  try {
    const models = await loadTestModels(scope);
    const objects = createGalaxyObjects(new Scene(), demoGalaxy, scope, models);
    const reference = createGalaxyObjects(new Scene(), demoGalaxy, scope, models);
    const star = demoGalaxy.stars[0];
    const shells = objects.systemsById.get(star.id).shells;
    const referenceShells = reference.systemsById.get(star.id).shells;
    objects.update(1, null);
    reference.update(2.5, { star, planet: null });
    shells.forEach((shell, index) => assert.deepEqual(shell.mesh.instanceMatrix.array, referenceShells[index].mesh.instanceMatrix.array));
    const paused = shells.map((shell) => shell.mesh.instanceMatrix.array.slice());
    const versions = shells.map((shell) => shell.mesh.instanceMatrix.version);
    objects.update(1, { star, planet: star.planets[0] });
    assert.deepEqual(
      shells.map((shell) => shell.mesh.instanceMatrix.array),
      paused,
    );
    assert.deepEqual(
      shells.map((shell) => shell.mesh.instanceMatrix.version),
      versions,
    );
    objects.update(0.5, { star, planet: null });
    reference.update(0.5, { star, planet: null });
    shells.forEach((shell, index) => assert.deepEqual(shell.mesh.instanceMatrix.array, referenceShells[index].mesh.instanceMatrix.array));
  } finally {
    assert.deepEqual(scope.dispose(), []);
  }
});
