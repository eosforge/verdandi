import assert from "node:assert/strict";
import test from "node:test";
import { Vector3 } from "three";
import { createPlanetOrbit, writeOrbitPosition } from "../src/features/galaxy/model/orbit.ts";
import { demoGalaxy } from "../src/features/galaxy/data/demo.ts";
import { createSystemPlane, maxPlanetInclination } from "../src/features/galaxy/model/orbitalPlane.ts";

// 测试直接使用生产轨道求解器, 转为向量只为比较几何量.
function positionAt(orbit, seconds) {
  const output = [0, 0, 0];
  writeOrbitPosition(orbit, seconds, output);
  return new Vector3(...output);
}

test("deterministic ellipses stay within the system plane band and retain the layout radius", () => {
  for (const star of demoGalaxy.stars) {
    const plane = createSystemPlane(star.id);
    const normal = new Vector3(...plane.normal);
    for (const planet of star.planets) {
      const orbit = createPlanetOrbit(planet.id, planet.position, plane);
      assert.deepEqual(orbit, createPlanetOrbit(planet.id, planet.position, plane));
      assert.ok(Math.abs(positionAt(orbit, 0).length() - Math.hypot(...planet.position)) < 1e-10);
      const orbitNormal = new Vector3(...orbit.periapsis).cross(new Vector3(...orbit.transverse));
      assert.ok(orbitNormal.angleTo(normal) <= maxPlanetInclination + 1e-12);
      assert.ok(orbitNormal.dot(normal) > 0, "all planets orbit in the same sense around their star");
      const period = (Math.PI * 2) / orbit.meanMotion;
      for (let index = 0; index < 32; index++) {
        const point = positionAt(orbit, (index / 32) * period);
        assert.ok(Math.abs(point.dot(normal)) <= point.length() * Math.sin(maxPlanetInclination) + 1e-10);
      }
      assert.ok(orbit.eccentricity >= 0.08 && orbit.eccentricity <= 0.32);
    }
  }
});

test("the star is a focus, motion stays in its plane and period obeys the semi-major axis", () => {
  for (const planet of demoGalaxy.stars[0].planets) {
    const orbit = createPlanetOrbit(planet.id, planet.position, createSystemPlane(planet.starId));
    const periapsis = new Vector3(...orbit.periapsis);
    const transverse = new Vector3(...orbit.transverse);
    const normal = periapsis.clone().cross(transverse);
    const secondFocus = periapsis.clone().multiplyScalar(-2 * orbit.semiMajorAxis * orbit.eccentricity);
    const period = (2 * Math.PI) / orbit.meanMotion;
    for (let step = 0; step < 24; step++) {
      const point = positionAt(orbit, (period * step) / 24);
      assert.ok(Math.abs(point.length() + point.distanceTo(secondFocus) - 2 * orbit.semiMajorAxis) < 1e-9);
      assert.ok(Math.abs(point.dot(normal)) < 1e-10);
    }
    assert.ok(Math.abs(orbit.meanMotion ** 2 * orbit.semiMajorAxis ** 3 - 0.028 ** 2 * 20 ** 3) < 1e-10);
    assert.ok(positionAt(orbit, period).distanceTo(positionAt(orbit, 0)) < 1e-10);
  }
});

test("periapsis is faster than apoapsis and swept-area rate is conserved", () => {
  const orbit = createPlanetOrbit("eccentric-sample", [20, 6, -8], createSystemPlane("test-star"));
  const step = 0.0001;
  const periapsisTime = -orbit.meanAnomaly / orbit.meanMotion;
  const apoapsisTime = (Math.PI - orbit.meanAnomaly) / orbit.meanMotion;
  const velocityAt = (seconds) =>
    positionAt(orbit, seconds + step)
      .sub(positionAt(orbit, seconds - step))
      .multiplyScalar(1 / (2 * step));
  const firstVelocity = velocityAt(periapsisTime);
  const secondVelocity = velocityAt(apoapsisTime);
  assert.ok(Math.abs(firstVelocity.length() / secondVelocity.length() - (1 + orbit.eccentricity) / (1 - orbit.eccentricity)) < 1e-7);
  const firstArea = positionAt(orbit, periapsisTime).cross(firstVelocity).length();
  const secondArea = positionAt(orbit, apoapsisTime).cross(secondVelocity).length();
  assert.ok(Math.abs(firstArea - secondArea) < 1e-7);
});

test("axis-aligned, stationary and long-running inputs stay finite", () => {
  for (const position of [
    [0, 0, 0],
    [0, 20, 0],
    [20, 0, 0],
  ]) {
    const orbit = createPlanetOrbit("axis-sample", position, createSystemPlane("test-star"));
    assert.ok(positionAt(orbit, 1e9).toArray().every(Number.isFinite));
    assert.ok(Math.abs(positionAt(orbit, 0).length() - Math.hypot(...position)) < 1e-10);
  }
});

test("system planes are orthonormal, deterministic by star ID and independent between stars", () => {
  const planes = demoGalaxy.stars.map((star) => createSystemPlane(star.id));
  assert.equal(new Set(planes.map((plane) => String(plane.normal))).size, planes.length);
  planes.forEach((plane, index) => {
    assert.deepEqual(plane, createSystemPlane(demoGalaxy.stars[index].id));
    const first = new Vector3(...plane.reference);
    const second = new Vector3(...plane.transverse);
    const normal = new Vector3(...plane.normal);
    for (const axis of [first, second, normal]) assert.ok(Math.abs(axis.length() - 1) < 1e-12);
    assert.ok(Math.abs(first.dot(second)) < 1e-12);
    assert.ok(first.cross(second).distanceTo(normal) < 1e-12);
  });
});
