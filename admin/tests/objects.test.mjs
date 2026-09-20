import assert from "node:assert/strict";
import { after, before, test } from "node:test";
import { Raycaster, Scene, Vector3, Quaternion } from "three";
import { demoGalaxy } from "../src/features/galaxy/data/demo.ts";
import { createGalaxyObjects } from "../src/features/galaxy/runtime/createGalaxyObjects.ts";
import { writeOrbitPosition } from "../src/features/galaxy/model/orbit.ts";
import { ResourceScope } from "../src/features/galaxy/runtime/resourceScope.ts";
import { loadTestModels } from "./support/celestialModels.mjs";
import { pulsarConfig } from "../src/features/galaxy/runtime/pulsar/config.ts";

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
    const scales = [0.75, 1.5, 4.5].map((scale) => scale * Math.cbrt(2));
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
      assert.deepEqual(system.group.position.toArray(), system.center);
      assert.ok(system.star === star, "layout must retain the caller's star identity and snapshot position");
      const planet = star.planets[0];
      const planetPosition = objects.worldPosition(objects.findPlanet(star.id, planet.id));
      const initial = [0, 0, 0];
      writeOrbitPosition(system.shells[0].orbits[0], 0, initial);
      assert.ok(
        planetPosition.distanceTo(new Vector3(...system.center).add(new Vector3(...initial))) < 1e-5,
        "world positions use the planned orbit without scaling the system frame",
      );
      const center = new Vector3(...system.center);
      const radius = 4.5 * scales[index];
      const inside = new Raycaster(center.clone().add(new Vector3(radius * 0.8, 0, 40)), new Vector3(0, 0, -1), 0, 80);
      const outside = new Raycaster(center.clone().add(new Vector3(radius * 1.2, 0, 40)), new Vector3(0, 0, -1), 0, 80);
      assert.equal(objects.pick(inside, null)?.star.id, star.id, "picking reaches the scaled surface");
      assert.equal(outside.intersectObject(system.surface, false).length, 0, "the surface does not retain the unscaled radius");
    });
    const blackHole = objects.systemsById.get("star-orion").surface;
    assert.deepEqual(
      blackHole.getWorldScale(new Vector3()).toArray(),
      [0.75, 0.75, 0.75].map((scale) => scale * Math.cbrt(2)),
    );
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
    for (const candidate of demoGalaxy.stars.filter((candidate) => candidate.status === "available" && candidate.appearance !== "pulsar")) {
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

test("elliptic motion retains 27 shared GLB batches and uploads each matrix buffer once per frame", async () => {
  const scope = new ResourceScope();
  try {
    const scene = new Scene();
    const models = await loadTestModels(scope);
    const objects = createGalaxyObjects(scene, demoGalaxy, scope, models);
    const meshes = [];
    scene.traverse((object) => {
      if (object.isInstancedMesh) meshes.push(object);
    });
    assert.equal(meshes.length, 27);
    assert.equal(
      meshes.reduce((sum, mesh) => sum + mesh.count, 0),
      252,
    );
    assert.equal(new Set(meshes.map((mesh) => mesh.geometry)).size, 1);
    assert.equal(meshes[0].geometry, models.planet.getObjectByName("Surface").geometry);
    for (const star of demoGalaxy.stars.filter((star) => star.status === "available" && star.appearance !== "pulsar"))
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

test("black-hole star only exposes its core, even when a snapshot retains old entities and links", async () => {
  const scope = new ResourceScope();
  try {
    const scene = new Scene();
    const snapshot = structuredClone(demoGalaxy);
    const star = snapshot.stars.find((star) => star.status === "black-hole");
    const planet = { ...snapshot.stars[0].planets[0], id: "orion/stale", starId: star.id };
    star.planets.push(planet);
    snapshot.links.push({ source: "star-atlas", target: star.id }, { source: star.id, target: "star-vega" });
    const before = structuredClone(snapshot);
    const objects = createGalaxyObjects(scene, snapshot, scope, await loadTestModels(scope));
    assert.equal(objects.findPlanet(star.id, planet.id), undefined);
    assert.equal(objects.systemsById.get(star.id).shells.length, 0);
    objects.showSelection({ star, planet });
    assert.equal(objects.selectionRing.visible, false);
    const center = new Vector3(...objects.systemsById.get(star.id).center);
    const starRay = new Raycaster(center.clone().add(new Vector3(0, 0, 6)), new Vector3(0, 0, -1), 0, 3);
    assert.deepEqual(objects.pick(starRay, null), { star, planet: null }, "black-hole core retains the star selection contract");
    const lines = scene.children.filter((object) => object.isLineSegments && object.name !== "StellarOrbitLines");
    assert.equal(lines.length, 1);
    assert.equal(lines[0].geometry.getAttribute("position").count, demoGalaxy.links.length * 2, "only links between available stars remain");
    const endpoints = lines[0].geometry.getAttribute("position");
    demoGalaxy.links.forEach((link, index) => {
      for (const [offset, id] of [
        [0, link.source],
        [1, link.target],
      ]) {
        const displayed = new Vector3(...objects.systemsById.get(id).center);
        const endpoint = new Vector3().fromBufferAttribute(endpoints, index * 2 + offset);
        assert.ok(endpoint.distanceTo(displayed) < 1e-4, "link endpoints follow the expanded star centers");
      }
    });
    for (const system of objects.systemsById.values())
      assert.ok(
        new Vector3(...system.center).distanceTo(objects.bounds.center) + system.orbitExtent <= objects.bounds.radius + 1e-8,
        "overview bounds contain every full orbital envelope",
      );
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
      .sub(new Vector3(...objects.systemsById.get(star.id).center))
      .normalize();
    const ray = new Raycaster(rotated.clone().addScaledVector(direction, 3), direction.negate(), 0, 4);
    assert.equal(objects.pick(ray, null)?.planet ?? null, null, "overview cannot select a planet, even within stellar click tolerance");
    assert.equal(objects.pick(ray, { star: snapshot.stars[1], planet: null })?.planet ?? null, null, "another star view cannot select this planet");
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

test("independent pulsar remains pickable and its spin can pause, reverse and change without following planet time", async () => {
  const scope = new ResourceScope();
  try {
    const objects = createGalaxyObjects(new Scene(), demoGalaxy, scope, await loadTestModels(scope));
    const system = objects.systemsById.get("star-pulsar");
    const rotor = system.group.getObjectByName("PulsarRotor");
    assert.deepEqual(
      system.surface.getWorldScale(new Vector3()).toArray(),
      [4, 4, 4],
      "pulsar model applies an eightfold multiplier to the zero-planet scale of 0.5",
    );
    const center = new Vector3(...system.center);
    const members = demoGalaxy.stars.filter((star) => star.appearance !== "pulsar");
    const mean = new Vector3();
    for (const member of members) mean.addScaledVector(new Vector3(...objects.systemsById.get(member.id).center), 1 / members.length);
    assert.ok(
      mean.distanceTo(center) < objects.bounds.radius * 0.2,
      "all stars including black-hole states contribute to the near-central equal-weight centroid",
    );
    assert.ok(objects.bounds.center.distanceTo(center) < 1e-9, "overview and orbit controls must use the common center");
    assert.equal(system.shells.length, 0);
    assert.equal(objects.pick(new Raycaster(center.clone().add(new Vector3(0, 0, 8)), new Vector3(0, 0, -1), 0, 10), null)?.star.id, "star-pulsar");
    const selection = { star: demoGalaxy.stars[0], planet: demoGalaxy.stars[0].planets[0] };
    objects.update(0.6, selection);
    const expected = new Quaternion().setFromAxisAngle(new Vector3(0, 1, 0), pulsarConfig.radiansPerSecond * 0.6);
    assert.ok(rotor.quaternion.angleTo(expected) < 1e-7);
    assert.equal(objects.setStarRotationSpeed(system.star.id, 0), true);
    const paused = rotor.quaternion.clone();
    objects.update(1, null);
    assert.deepEqual(rotor.quaternion, paused);
    assert.equal(objects.setStarRotationSpeed(system.star.id, -pulsarConfig.radiansPerSecond), true);
    objects.update(0.6, selection);
    assert.ok(rotor.quaternion.angleTo(new Quaternion()) < 1e-7);
  } finally {
    assert.deepEqual(scope.dispose(), []);
  }
});

test("overview and star clocks use 15:6 rates; planet selection pauses without uploading or resetting poses", async () => {
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
    const centers = [...objects.systemsById.values()].map((system) => [...system.center]);
    shells.forEach((shell, index) => assert.deepEqual(shell.mesh.instanceMatrix.array, referenceShells[index].mesh.instanceMatrix.array));
    const paused = shells.map((shell) => shell.mesh.instanceMatrix.array.slice());
    const versions = shells.map((shell) => shell.mesh.instanceMatrix.version);
    objects.update(1, { star, planet: star.planets[0] });
    assert.deepEqual(
      [...objects.systemsById.values()].map((system) => system.center),
      centers,
      "planet selection also pauses stellar revolution",
    );
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

test("stellar revolution translates whole systems and updates all link endpoints without modifying the snapshot", async () => {
  const scope = new ResourceScope();
  try {
    const scene = new Scene();
    const snapshot = structuredClone(demoGalaxy);
    const objects = createGalaxyObjects(scene, snapshot, scope, await loadTestModels(scope));
    const centers = new Map([...objects.systemsById].map(([id, system]) => [id, [...system.center]]));
    const lines = scene.children.find((object) => object.isLineSegments);
    lines.visible = true; // 显式启用连线, 本用例继续覆盖其动态端点同步.
    const positions = lines.geometry.getAttribute("position");
    const version = positions.version;
    objects.update(4, null);
    assert.equal(positions.version, version + 1);
    for (const [id, system] of objects.systemsById) {
      assert.deepEqual(system.group.position.toArray(), system.center);
      const stationary = system.star.appearance === "pulsar";
      assert.equal(new Vector3(...centers.get(id)).distanceTo(new Vector3(...system.center)) < 1e-9, stationary);
      assert.ok(new Vector3(...system.center).distanceTo(objects.bounds.center) + system.orbitExtent <= objects.bounds.radius + 1e-8);
    }
    snapshot.links.forEach((link, index) => {
      for (const [offset, id] of [
        [0, link.source],
        [1, link.target],
      ])
        assert.ok(new Vector3().fromBufferAttribute(positions, index * 2 + offset).distanceTo(new Vector3(...objects.systemsById.get(id).center)) < 1e-3);
    });
    assert.deepEqual(snapshot, demoGalaxy);
  } finally {
    assert.deepEqual(scope.dispose(), []);
  }
});
