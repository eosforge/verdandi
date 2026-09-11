import assert from "node:assert/strict";
import test from "node:test";
import { Vector3 } from "three";
import { CameraMotion } from "../src/features/galaxy/runtime/cameraMotion.ts";

test("peer focus starts at the current pose, eases, and ends at a copied destination", () => {
  const position = new Vector3(0, 0, 100);
  const target = new Vector3();
  const motion = new CameraMotion(position, target);
  const destination = new Vector3(50, 20, 90);
  motion.moveTo(destination, new Vector3(50, 20, 0));
  destination.set(999, 999, 999);
  assert.deepEqual(position.toArray(), [0, 0, 100]);
  motion.update(0.1);
  assert.ok(position.x > 0 && position.x < 1);
  motion.update(0.5);
  assert.ok(position.distanceTo(new Vector3(25, 10, 95)) < 1e-10);
  motion.update(0.6);
  assert.deepEqual(position.toArray(), [50, 20, 90]);
  assert.deepEqual(target.toArray(), [50, 20, 0]);
});

test("redirect and drag interruption keep the current camera pose", () => {
  const position = new Vector3(0, 0, 100);
  const target = new Vector3();
  const motion = new CameraMotion(position, target);
  motion.moveTo(new Vector3(100, 0, 100), new Vector3(100, 0, 0));
  motion.update(0.4);
  const current = position.clone();
  motion.moveTo(new Vector3(-50, 0, 100), new Vector3(-50, 0, 0));
  assert.deepEqual(position, current);
  motion.cancel();
  motion.update(1);
  assert.deepEqual(position, current);
});

function zoomAt(refreshRate) {
  const position = new Vector3(0, 0, 100);
  const target = new Vector3();
  const anchor = new Vector3(15, 8, 0);
  const motion = new CameraMotion(position, target);
  const anchorDirection = position.clone().sub(anchor).normalize();
  motion.zoomBy(-120, anchor);
  motion.zoomBy(-120, anchor);
  assert.equal(position.z, 100, "wheel input only changes the destination");
  let lastDistance = 100;
  for (let frame = 0; frame < refreshRate / 2; frame++) {
    motion.update(1 / refreshRate);
    const distance = position.distanceTo(target);
    assert.ok(distance < lastDistance && distance > 60);
    assert.ok(position.clone().sub(anchor).normalize().distanceTo(anchorDirection) < 1e-10);
    lastDistance = distance;
  }
  return { position, target, motion };
}

test("wheel zoom is monotonic, preserves its anchor, and is independent of frame rate", () => {
  const at30 = zoomAt(30);
  const at60 = zoomAt(60);
  const at120 = zoomAt(120);
  assert.ok(at30.position.distanceTo(at60.position) < 1e-9);
  assert.ok(at120.position.distanceTo(at60.position) < 1e-9);
  const current = at60.position.clone();
  at60.motion.pause();
  at60.motion.update(0.1);
  assert.deepEqual(at60.position, current);
});

test("repeated wheel input remains inside the configured distance bounds", () => {
  const position = new Vector3(0, 0, 100);
  const target = new Vector3();
  const motion = new CameraMotion(position, target);
  for (const [pixels, bound] of [
    [-10000, 8],
    [10000, 440],
  ]) {
    for (let index = 0; index < 100; index++) motion.zoomBy(pixels, target);
    for (let frame = 0; frame < 240; frame++) motion.update(1 / 60);
    assert.ok(Math.abs(position.distanceTo(target) - bound) < 1e-9);
  }
});
