import assert from "node:assert/strict";
import { test } from "node:test";
import { Group, InstancedMesh, Mesh, MeshBasicMaterial, Raycaster, Scene, SphereGeometry, Vector3 } from "three";
import { createObjectIndex } from "../src/features/galaxy/runtime/objects/createObjectIndex.ts";

// 使用小型真实网格隔离区域拾取, 无需加载 GLB 或创建浏览器上下文.
function fixture(t, entries) {
  const scene = new Scene();
  const geometry = new SphereGeometry(1, 16, 12);
  const material = new MeshBasicMaterial();
  const systems = entries.map(({ id, center = [0, 0, 0], radius = 10, bodyRadius = 1, planets = true, status = "available", appearance = "star" }) => {
    const star = { id, status, appearance, planets: planets ? [{ id: `${id}/planet` }] : [] };
    const group = new Group();
    group.position.set(...center);
    const surface = new Mesh(geometry, material);
    const mesh = new InstancedMesh(geometry, material, 1);
    // 行星位于 x=7, 与测试中的空白区域射线分离.
    mesh.position.x = 7;
    group.add(surface, mesh);
    scene.add(group);
    t.after(() => mesh.dispose());
    return {
      star,
      center: [...center],
      surface,
      group,
      shells: planets ? [{ mesh, planets: star.planets, orbits: [] }] : [],
      orbitExtent: radius,
      bodyRadius,
    };
  });
  t.after(() => {
    geometry.dispose();
    material.dispose();
  });
  return { systems, index: createObjectIndex(scene, systems) };
}

const ray = (x, z = 30, near = 0, far = Infinity) => new Raycaster(new Vector3(x, 0, z), new Vector3(0, 0, -1), near, far);

test("stellar click diameter is tripled independently of orbital extent and follows world transforms", (t) => {
  const {
    index,
    systems: [system],
  } = fixture(t, [{ id: "a" }]);
  const selected = { star: system.star, planet: null };
  assert.deepEqual(index.pick(ray(2.9), null), selected);
  assert.equal(index.pick(ray(3.1), null), null);
  assert.deepEqual(index.pick(ray(2.9), selected), selected);
  assert.equal(index.pick(ray(7), null), null);
  assert.equal(index.pick(ray(7), selected)?.planet?.id, "a/planet");
  system.center[0] = 40;
  system.group.position.x = 40;
  assert.equal(index.pick(ray(2.9), null), null, "the previous region no longer responds");
  assert.deepEqual(index.pick(ray(42.9), null), selected, "the region follows stellar revolution");
  system.surface.scale.setScalar(2);
  assert.deepEqual(index.pick(ray(45.9), null), selected, "click radius follows the scaled core");
  assert.equal(index.pick(ray(46.1), null), null);
});

test("exact stellar bodies win over regions; overlapping regions use nearest entry, independent of ordering", (t) => {
  const entries = [
    { id: "front", center: [0, 0, 10] },
    { id: "back", center: [2, 0, -20] },
  ];
  for (const ordered of [entries, [...entries].reverse()]) {
    const { index } = fixture(t, ordered);
    assert.equal(index.pick(ray(2), null)?.star.id, "back", "a foreground region must not block a stellar body");
    assert.equal(index.pick(ray(-1.5), null)?.star.id, "front");
    const overlap = new Raycaster(new Vector3(1, 2, 30), new Vector3(0, 0, -1));
    assert.equal(index.pick(overlap, null)?.star.id, "front");
  }
  const { index } = fixture(t, [{ id: "no-planets", radius: 100, planets: false }]);
  assert.equal(index.pick(ray(2.9), null)?.star.id, "no-planets", "planet count does not control click tolerance");
  assert.equal(index.pick(ray(5), null), null, "orbital extent does not control click tolerance");
});

test("black holes use tripled full optical diameter, while pulsars retain their actual core size", (t) => {
  const { index, systems } = fixture(t, [
    { id: "hole", status: "black-hole", planets: false, bodyRadius: 6 },
    { id: "pulse", appearance: "pulsar", center: [20, 0, 0], planets: false },
  ]);
  assert.equal(index.pick(ray(17.9), null)?.star.id, "hole", "the accretion region sets the base diameter, not the tiny core");
  assert.equal(index.pick(ray(18.1), null), null);
  assert.equal(index.pick(ray(20), null)?.star.id, "pulse");
  assert.equal(index.pick(ray(22), null), null);
  systems[0].center[0] = -40;
  systems[0].group.position.x = -40;
  assert.equal(index.pick(ray(17.9), null), null);
  assert.equal(index.pick(ray(-57.9), null)?.star.id, "hole", "the full optical region follows the live stellar center");
});

test("click tolerance respects clipping and works with the camera or entire ray segment inside", (t) => {
  const { index } = fixture(t, [{ id: "a" }]);
  assert.equal(index.pick(ray(2, -20), null), null, "region behind camera");
  assert.equal(index.pick(ray(2, 30, 0, 10), null), null, "beyond far clip");
  assert.equal(index.pick(ray(2, 30, 45), null), null, "before near clip");
  assert.equal(index.pick(ray(2, 0, 0, 1), null)?.star.id, "a", "ray entirely inside region");
  assert.equal(index.pick(ray(2, 30, 29, 30), null)?.star.id, "a", "clipped segment inside region");
});
