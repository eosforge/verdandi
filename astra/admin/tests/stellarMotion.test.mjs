// 验证新增恒星倍率与原有卫星时钟分离, 不创建 WebGL 或模型资产.
import assert from "node:assert/strict";
import test from "node:test";
import { Group } from "three";
import { createObjectMotion } from "../src/features/galaxy/runtime/objects/createObjectMotion.ts";
import { writeStellarPosition } from "../src/features/galaxy/model/stellarOrbits.ts";

test("stellar revolution stays at fifteen in both overview and star view, and detail selection pauses it", () => {
  const star = { id: "s", name: "S", status: "black-hole", position: [0, 0, 0], planets: [] };
  const orbit = {
    centerId: "p",
    center: [0, 0, 0],
    extent: 100,
    members: [
      {
        id: "s",
        path: { semiMajorAxis: 50, eccentricity: 0.03, periapsis: [1, 0, 0], transverse: [0, 0, 1], meanAnomaly: 0.4, meanMotion: (Math.PI * 2) / 600 },
      },
    ],
  };
  const system = { star, center: [0, 0, 0], group: new Group(), shells: [] };
  const motion = createObjectMotion({ systems: [system], blackHoles: [], pulsars: [], rotatingStars: new Map(), stellarOrbit: orbit });
  const expected = [0, 0, 0];
  motion.update(1, null);
  writeStellarPosition(orbit, 0, 15, expected);
  assert.deepEqual(system.center, expected, "overview uses an independent 15x stellar clock");
  motion.update(1, { star, planet: null });
  writeStellarPosition(orbit, 0, 30, expected);
  assert.deepEqual(system.center, expected, "selection does not change the stellar speed");
  motion.update(1, { star, planet: { id: "selected" } });
  assert.deepEqual(system.center, expected);
  motion.update(1, null);
  writeStellarPosition(orbit, 0, 45, expected);
  assert.deepEqual(system.center, expected, "returning to overview resumes at the same speed without resetting phase");
});
