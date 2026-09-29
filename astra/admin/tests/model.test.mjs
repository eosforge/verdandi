import assert from "node:assert/strict";
import test from "node:test";
import { demoGalaxy } from "../src/features/galaxy/data/demo.ts";
import { diskPosition } from "../src/features/galaxy/model/layout.ts";
import { createSystemPlane } from "../src/features/galaxy/model/orbitalPlane.ts";
import { starScaleForPlanetCount } from "../src/features/galaxy/model/presentation.ts";
import { validateGalaxyData } from "../src/features/galaxy/model/validate.ts";
import { createGalaxyScene } from "../src/features/galaxy/runtime/createGalaxyScene.ts";

test("star scale follows the requested piecewise count landmarks and clamps both endpoints", () => {
  for (const [count, scale] of [
    [0, 0.5],
    [1, 0.5],
    [10, 0.5],
    [20, 0.75],
    [25, 0.875],
    [30, 1],
    [45, 1.25],
    [60, 1.5],
    [90, 1.75],
    [120, 2],
    [150, 2.5],
    [180, 3],
    [1000, 3],
  ])
    assert.equal(starScaleForPlanetCount(count), scale, `${count} planets`);
  for (let count = 1; count <= 200; count++) {
    const previous = starScaleForPlanetCount(count - 1);
    const current = starScaleForPlanetCount(count);
    assert.ok(current >= previous && current - previous <= 0.025 + 1e-12, "size is continuous and monotonic across breakpoints");
  }
});

test("demo contains nine connected ordinary stars, an isolated black hole and an isolated pulsar", () => {
  validateGalaxyData(demoGalaxy);
  assert.deepEqual(
    demoGalaxy.stars.map((star) => star.planets.length),
    [36, 48, 60, 12, 15, 18, 21, 24, 18, 0, 0],
  );
  assert.equal(demoGalaxy.links.length, 36);
  assert.ok(demoGalaxy.links.every((link) => link.source !== "star-orion" && link.target !== "star-orion"));
  assert.ok(demoGalaxy.links.every((link) => link.source !== "star-pulsar" && link.target !== "star-pulsar"));
  assert.equal(demoGalaxy.stars.find((star) => star.id === "star-pulsar").appearance, "pulsar");
  const members = demoGalaxy.stars.filter((star) => star.appearance !== "pulsar");
  const central = demoGalaxy.stars.find((star) => star.id === demoGalaxy.centerStarId);
  for (let axis = 0; axis < 3; axis++)
    assert.ok(Math.abs(central.position[axis] - members.reduce((sum, star) => sum + star.position[axis] / members.length, 0)) < 1e-10);
  assert.deepEqual(
    demoGalaxy.stars.filter((star) => star.status === "available" && star.appearance !== "pulsar").map((star) => star.id),
    ["star-atlas", "star-lyra", "star-vega", "star-sirius", "star-capella", "star-rigel", "star-procyon", "star-altair", "star-deneb"],
  );
  assert.deepEqual(
    demoGalaxy.stars.filter((star) => star.status === "black-hole").map((star) => star.id),
    ["star-orion"],
  );
  validateGalaxyData({ stars: [], links: [], sourceLabel: "", description: "" });
});

test("disk layout is deterministic, finite, and stays on its requested plane and radius", () => {
  const plane = createSystemPlane("layout-test-star");
  for (const count of [1, 12, 100, 1000]) {
    const positions = Array.from({ length: count }, (_, index) => diskPosition(index, count, 27, plane, 0.85));
    assert.equal(new Set(positions.map(String)).size, count);
    positions.forEach((position, index) => {
      assert.ok(Math.abs(Math.hypot(...position) - 27) < 1e-10);
      assert.deepEqual(position, diskPosition(index, count, 27, plane, 0.85));
      assert.ok(Math.abs(position.reduce((sum, value, axis) => sum + value * plane.normal[axis], 0)) < 1e-10);
    });
  }
  for (const args of [
    [0, 0, 1],
    [1, 1, 1],
    [-1, 3, 1],
    [0, 1, 0],
    [0, 1, Infinity],
    [0, 1, 1, NaN],
  ]) {
    assert.throws(() => diskPosition(args[0], args[1], args[2], plane, args[3]), RangeError);
  }
});

for (const [name, mutate] of [
  [
    "invalid availability",
    (data) => {
      data.stars[0].status = "unknown";
    },
  ],
  ["duplicate star", (data) => data.stars.push(data.stars[0])],
  [
    "missing overview center",
    (data) => {
      data.centerStarId = "missing";
    },
  ],
  [
    "invalid star appearance",
    (data) => {
      data.stars[0].appearance = "unknown";
    },
  ],
  ["duplicate planet", (data) => data.stars[0].planets.push(data.stars[0].planets[0])],
  [
    "wrong owner",
    (data) => {
      data.stars[0].planets[0].starId = "missing";
    },
  ],
  [
    "unknown kind",
    (data) => {
      data.stars[0].planets[0].kind = "Other";
    },
  ],
  [
    "nonfinite position",
    (data) => {
      data.stars[0].position[0] = NaN;
    },
  ],
  [
    "invalid color",
    (data) => {
      data.stars[0].color = "broken";
    },
  ],
  [
    "dangling link",
    (data) => {
      data.links[0].target = "missing";
    },
  ],
  [
    "self link",
    (data) => {
      data.links[0].target = data.links[0].source;
    },
  ],
  ["reversed duplicate link", (data) => data.links.push({ source: data.links[0].target, target: data.links[0].source })],
]) {
  test(`${name} is rejected before accessing the DOM or GPU`, async () => {
    const invalid = structuredClone(demoGalaxy);
    mutate(invalid);
    assert.throws(() => validateGalaxyData(invalid));
    await assert.rejects(
      () => createGalaxyScene(null, invalid, {}),
      (error) => {
        assert.doesNotMatch(error.message, /document|WebGL|append/);
        return true;
      },
    );
  });
}
