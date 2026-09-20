// 径向带解析间距与独立相位采样共同覆盖规划输出, 不依赖重复周期或候选搜索实现.
import assert from "node:assert/strict";
import test from "node:test";
import { Vector3 } from "three";
import { demoGalaxy } from "../src/features/galaxy/data/demo.ts";
import { planPlanetOrbits, orbitSpacing } from "../src/features/galaxy/model/orbitPlanner.ts";
import { writeOrbitPosition } from "../src/features/galaxy/model/orbit.ts";
import { createSystemPlane, maxPlanetInclination } from "../src/features/galaxy/model/orbitalPlane.ts";
import { spaceStarSystems, systemSpacing } from "../src/features/galaxy/model/systemLayout.ts";
import { starScaleForPlanetCount } from "../src/features/galaxy/model/presentation.ts";

const radius = 0.82;
const requests = demoGalaxy.stars.flatMap((star) => star.planets.map((planet) => ({ id: planet.id, starId: star.id, position: planet.position })));
const systems = demoGalaxy.stars.map((star) => ({
  id: star.id,
  radius: star.status === "available" ? 4.6 * starScaleForPlanetCount(star.planets.length) : 3.4,
}));
const planned = planPlanetOrbits(requests, radius, systems);
const envelopes = demoGalaxy.stars.map((star) => ({ id: star.id, position: star.position, extent: Math.max(24, planned.extents.get(star.id)) }));
const centers = spaceStarSystems(envelopes);

test("planning is deterministic, preserves its inputs and assigns distinct axes with independent Kepler periods", () => {
  const before = structuredClone(requests);
  const reversed = planPlanetOrbits([...requests].reverse(), radius, [...systems].reverse());
  assert.deepEqual([...reversed.orbits], [...planned.orbits]);
  assert.deepEqual(requests, before);
  for (const system of systems) {
    const orbits = requests
      .filter((request) => request.starId === system.id)
      .map((request) => planned.orbits.get(request.id))
      .sort((a, b) => a.semiMajorAxis - b.semiMajorAxis);
    assert.equal(new Set(orbits.map((orbit) => orbit.semiMajorAxis)).size, orbits.length);
    let outer = system.radius - radius;
    let previousMotion = Infinity;
    for (const orbit of orbits) {
      assert.ok(orbit.semiMajorAxis * (1 - orbit.eccentricity) - outer >= 2 * radius + orbitSpacing.surfaceGap - 1e-10);
      assert.ok(orbit.meanMotion < previousMotion);
      assert.ok(Math.abs(orbit.meanMotion ** 2 * orbit.semiMajorAxis ** 3 - 0.028 ** 2 * 20 ** 3) < 1e-10);
      const normal = new Vector3(...orbit.periapsis).cross(new Vector3(...orbit.transverse));
      assert.ok(normal.angleTo(new Vector3(...createSystemPlane(system.id).normal)) <= maxPlanetInclination + 1e-12);
      outer = orbit.semiMajorAxis * (1 + orbit.eccentricity);
      previousMotion = orbit.meanMotion;
    }
  }
});

test("all demo planets remain clear of other planets and stellar cores even at independent phases", () => {
  const points = requests.map(() => [0, 0, 0]);
  let minimum = Infinity;
  for (let sample = 0; sample < 257; sample++) {
    requests.forEach((request, index) => {
      const orbit = planned.orbits.get(request.id);
      const center = centers.get(request.starId);
      const period = (Math.PI * 2) / orbit.meanMotion;
      writeOrbitPosition(orbit, ((sample / 257) * 11 + index * 0.371) * period, points[index]);
      for (let axis = 0; axis < 3; axis++) points[index][axis] += center[axis];
    });
    for (let index = 0; index < points.length; index++) {
      for (let other = index + 1; other < points.length; other++)
        minimum = Math.min(minimum, Math.hypot(...points[index].map((value, axis) => value - points[other][axis])) - radius * 2);
      for (const system of systems) {
        const center = centers.get(system.id);
        minimum = Math.min(minimum, Math.hypot(...points[index].map((value, axis) => value - center[axis])) - radius - system.radius);
      }
    }
  }
  assert.ok(minimum >= orbitSpacing.surfaceGap - 1e-8, "independent phases cannot bypass radial or system separation");
});

test("coincident hints receive separate radial bands instead of sharing spatial paths", () => {
  const bodies = Array.from({ length: 180 }, (_, index) => ({ id: "planet/" + index, starId: "star", position: [13, 0, 0] }));
  const plan = planPlanetOrbits(bodies, radius, [{ id: "star", radius: 14 }]);
  const orbits = [...plan.orbits.values()].sort((a, b) => a.semiMajorAxis - b.semiMajorAxis);
  assert.equal(new Set(orbits.map((orbit) => orbit.semiMajorAxis)).size, 180);
  assert.ok(orbits.at(-1).semiMajorAxis < 800, "bounded radial excursion avoids exponential expansion");
  for (let index = 1; index < orbits.length; index++)
    assert.ok(
      orbits[index].semiMajorAxis * (1 - orbits[index].eccentricity) - orbits[index - 1].semiMajorAxis * (1 + orbits[index - 1].eccentricity) >=
        2 * radius + orbitSpacing.surfaceGap - 1e-9,
    );
  assert.equal(planPlanetOrbits([], radius, systems).orbits.size, 0);
  assert.throws(() => planPlanetOrbits(bodies, radius, []), /Missing orbital system/);
});

test("system spacing preserves direction, leaves positive margins and handles coincident centers deterministically", () => {
  const before = structuredClone(envelopes);
  assert.deepEqual([...spaceStarSystems([...envelopes].reverse())], [...centers]);
  assert.deepEqual(envelopes, before);
  for (let index = 0; index < envelopes.length; index++)
    for (let other = index + 1; other < envelopes.length; other++) {
      const a = envelopes[index],
        b = envelopes[other];
      const displayed = new Vector3(...centers.get(b.id)).sub(new Vector3(...centers.get(a.id)));
      const original = new Vector3(...b.position).sub(new Vector3(...a.position));
      assert.ok(displayed.clone().normalize().distanceTo(original.normalize()) < 1e-12);
      assert.ok(displayed.length() >= (a.extent + b.extent) * systemSpacing.extentRatio + systemSpacing.gap - 1e-8);
    }
  const coincident = envelopes.map((envelope) => ({ ...envelope, position: [0, 0, 0] }));
  const spaced = spaceStarSystems(coincident);
  assert.deepEqual([...spaced], [...spaceStarSystems([...coincident].reverse())]);
  assert.equal(new Set([...spaced.values()].map(String)).size, coincident.length);
  assert.equal(spaceStarSystems([]).size, 0);
});
