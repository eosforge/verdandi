import assert from "node:assert/strict";
import test from "node:test";
import { demoGalaxy } from "../src/features/galaxy/data/demo.ts";
import { planPlanetOrbits } from "../src/features/galaxy/model/orbitPlanner.ts";
import { writeOrbitPosition } from "../src/features/galaxy/model/orbit.ts";
import { starScaleForPlanetCount } from "../src/features/galaxy/model/presentation.ts";

const radius = 0.82;
const requests = demoGalaxy.stars.flatMap((star) => star.planets.map((planet) => ({ id: planet.id, position: planet.position, center: star.position })));
const obstacles = demoGalaxy.stars.map((star) => ({
  center: star.position,
  radius: star.status === "available" ? 4.6 * starScaleForPlanetCount(star.planets.length) : 3.4,
}));
const planned = planPlanetOrbits(requests, radius, obstacles);

// 使用与规划器不同的采样数量和偏移检查整个周期, 并覆盖跨星系、跨类型以及恒星核心.
function minimumClearance(plan, bodies, bodyRadius, blockers, count) {
  const points = bodies.map(() => [0, 0, 0]);
  let gap = Infinity;
  for (let sample = 0; sample < count; sample++) {
    const time = ((sample + 0.371) / count) * plan.period;
    bodies.forEach((body, index) => {
      writeOrbitPosition(plan.orbits.get(body.id), time, points[index]);
      for (let axis = 0; axis < 3; axis++) points[index][axis] += body.center[axis];
    });
    points.forEach((point, index) => {
      for (let other = index + 1; other < points.length; other++)
        gap = Math.min(gap, Math.hypot(...point.map((value, axis) => value - points[other][axis])) - bodyRadius * 2);
      for (const blocker of blockers)
        gap = Math.min(gap, Math.hypot(...point.map((value, axis) => value - blocker.center[axis])) - bodyRadius - blocker.radius);
    });
  }
  return gap;
}

test("orbit planning is deterministic by ID, retains Kepler periods and does not mutate the layout hints", () => {
  const copy = structuredClone(requests);
  const reverse = planPlanetOrbits([...requests].reverse(), radius, obstacles);
  assert.equal(reverse.period, planned.period);
  assert.deepEqual([...reverse.orbits], [...planned.orbits]);
  assert.deepEqual(requests, copy);
  for (const orbit of planned.orbits.values()) {
    const harmonic = (orbit.meanMotion * planned.period) / (Math.PI * 2);
    assert.ok(Math.abs(harmonic - Math.round(harmonic)) < 1e-12);
    assert.ok(Math.abs(orbit.meanMotion ** 2 * orbit.semiMajorAxis ** 3 - 0.028 ** 2 * 20 ** 3) < 1e-10);
    const before = [0, 0, 0];
    const after = [0, 0, 0];
    writeOrbitPosition(orbit, 0.73, before);
    writeOrbitPosition(orbit, planned.period * 11 + 0.73, after);
    assert.ok(Math.hypot(...after.map((value, index) => value - before[index])) < 1e-9, "the checked configuration repeats without phase drift");
  }
});

test("all 144 demo planets stay separated from each other and every stellar core over the complete repeat cycle", (context) => {
  const gap = minimumClearance(planned, requests, radius, obstacles, 2053);
  assert.ok(gap > 0.1, `minimum surface clearance ${gap}`);
  context.diagnostic(`144 planets, full period ${planned.period.toFixed(2)} virtual seconds, sampled minimum surface clearance ${gap.toFixed(3)}`);
  assert.ok(Math.max(...[...planned.orbits.values()].map((orbit) => orbit.semiMajorAxis * (1 + orbit.eccentricity))) < 50, "avoidance keeps this demo compact");
});

test("coincident layout hints are rephased, and spatially crossing paths are permitted at different times", () => {
  const bodies = Array.from({ length: 12 }, (_, index) => ({ id: `crossing/${index}`, position: [13, 0, 0], center: [0, 0, 0] }));
  const blockers = [{ center: [0, 0, 0], radius: 6 }];
  const plan = planPlanetOrbits(bodies, radius, blockers);
  assert.ok(minimumClearance(plan, bodies, radius, blockers, 1021) > 0.1);
  const points = [0, 0, 0];
  const other = [0, 0, 0];
  const orbits = [...plan.orbits.values()];
  let sharedSpace = false;
  for (let first = 0; first < orbits.length && !sharedSpace; first++) {
    for (let second = first + 1; second < orbits.length && !sharedSpace; second++) {
      for (let a = 0; a < 96 && !sharedSpace; a++) {
        writeOrbitPosition(orbits[first], (a / 96) * ((Math.PI * 2) / orbits[first].meanMotion), points);
        for (let b = 0; b < 96; b++) {
          writeOrbitPosition(orbits[second], (b / 96) * ((Math.PI * 2) / orbits[second].meanMotion), other);
          if (Math.hypot(...points.map((value, axis) => value - other[axis])) < radius * 2) {
            sharedSpace = true;
            break;
          }
        }
      }
    }
  }
  assert.equal(sharedSpace, true, "the planner checks synchronous movement rather than reserving entire orbit tubes");
  assert.equal(planPlanetOrbits([], radius, blockers).orbits.size, 0);
});
