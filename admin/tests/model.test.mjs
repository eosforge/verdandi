import assert from "node:assert/strict";
import test from "node:test";
import { demoGalaxy } from "../src/features/galaxy/data/demo.ts";
import { spherePosition } from "../src/features/galaxy/model/layout.ts";
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

test("demo retains the three available systems and an isolated unavailable peer", () => {
  validateGalaxyData(demoGalaxy);
  assert.deepEqual(
    demoGalaxy.peers.map((peer) => peer.planets.length),
    [36, 48, 60, 0],
  );
  assert.equal(demoGalaxy.links.length, 3);
  assert.ok(demoGalaxy.links.every((link) => link.source !== "peer-orion" && link.target !== "peer-orion"));
  assert.deepEqual(
    demoGalaxy.peers.filter((peer) => peer.status === "available").map((peer) => peer.id),
    ["peer-atlas", "peer-lyra", "peer-vega"],
  );
  assert.deepEqual(
    demoGalaxy.peers.filter((peer) => peer.status === "unavailable").map((peer) => peer.id),
    ["peer-orion"],
  );
  validateGalaxyData({ ...demoGalaxy, peers: [], links: [] });
});

test("spherical layout is deterministic, finite, and stays on its requested radius", () => {
  for (const count of [1, 12, 100, 1000]) {
    const positions = Array.from({ length: count }, (_, index) => spherePosition(index, count, 27, 0.85));
    assert.equal(new Set(positions.map(String)).size, count);
    positions.forEach((position, index) => {
      assert.ok(Math.abs(Math.hypot(...position) - 27) < 1e-10);
      assert.deepEqual(position, spherePosition(index, count, 27, 0.85));
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
    assert.throws(() => spherePosition(...args), RangeError);
  }
});

for (const [name, mutate] of [
  [
    "invalid availability",
    (data) => {
      data.peers[0].status = "unknown";
    },
  ],
  ["duplicate peer", (data) => data.peers.push(data.peers[0])],
  ["duplicate planet", (data) => data.peers[0].planets.push(data.peers[0].planets[0])],
  [
    "wrong owner",
    (data) => {
      data.peers[0].planets[0].peerId = "missing";
    },
  ],
  [
    "unknown kind",
    (data) => {
      data.peers[0].planets[0].kind = "Other";
    },
  ],
  [
    "nonfinite position",
    (data) => {
      data.peers[0].position[0] = NaN;
    },
  ],
  [
    "invalid color",
    (data) => {
      data.peers[0].color = "broken";
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
