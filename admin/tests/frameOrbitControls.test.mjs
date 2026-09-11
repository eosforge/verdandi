import assert from "node:assert/strict";
import test from "node:test";
import { PerspectiveCamera, Vector3 } from "three";
import { CameraMotion } from "../src/features/galaxy/runtime/cameraMotion.ts";
import { FrameOrbitControls } from "../src/features/galaxy/runtime/frameOrbitControls.ts";

function createControls() {
  const document = new EventTarget();
  const canvas = new EventTarget();
  Object.assign(canvas, { ownerDocument: document, getRootNode: () => document, style: {}, clientWidth: 800, clientHeight: 600 });
  const camera = new PerspectiveCamera(45, 800 / 600, 0.1, 1600);
  camera.position.set(0, 40, 90);
  const controls = new FrameOrbitControls(camera, canvas);
  controls.enableDamping = true;
  controls.updateFrame(0);
  camera.updateMatrixWorld();
  return { camera, controls };
}

function dragWithEvents(eventsPerFrame) {
  const { camera, controls } = createControls();
  const poses = [];
  try {
    for (let frame = 0; frame < 60; frame++) {
      const before = camera.position.clone();
      const targetBefore = controls.target.clone();
      for (let event = 0; event < eventsPerFrame; event++) {
        controls.rotateLeft(0.01 / eventsPerFrame);
        controls.pan(1 / eventsPerFrame, 0.25 / eventsPerFrame);
      }
      assert.deepEqual(camera.position, before, "input must not move the camera between draws");
      assert.deepEqual(controls.target, targetBefore);
      controls.updateFrame(1 / 60);
      camera.updateMatrixWorld();
      poses.push({ position: camera.position.clone(), target: controls.target.clone() });
    }
    return poses;
  } finally {
    controls.dispose();
  }
}

test("uneven mouse event batches produce the same per-frame camera path", () => {
  const single = dragWithEvents(1);
  const batched = dragWithEvents(16);
  single.forEach((pose, index) => {
    assert.ok(pose.position.distanceTo(batched[index].position) < 1e-10);
    assert.ok(pose.target.distanceTo(batched[index].target) < 1e-10);
  });
});

test("rotation and pan damping converge by elapsed time at 30, 60 and 120 FPS", () => {
  const results = [30, 60, 120].map((fps) => {
    const { camera, controls } = createControls();
    try {
      controls.rotateLeft(0.6);
      controls.pan(100, 25);
      let previousAngle = controls.getAzimuthalAngle();
      for (let frame = 0; frame < fps / 2; frame++) {
        controls.updateFrame(1 / fps);
        const angle = controls.getAzimuthalAngle();
        assert.ok(angle <= previousAngle && angle >= -0.600001, "drag must settle without oscillation or overshoot");
        previousAngle = angle;
      }
      return { position: camera.position.clone(), target: controls.target.clone() };
    } finally {
      controls.dispose();
    }
  });
  for (const result of results) {
    assert.ok(result.position.distanceTo(results[0].position) < 1e-10);
    assert.ok(result.target.distanceTo(results[0].target) < 1e-10);
  }
});

test("focus clears residual drag without jumping and reaches the requested pose without recoil", () => {
  const { camera, controls } = createControls();
  try {
    controls.rotateLeft(0.8);
    controls.pan(100, 25);
    controls.updateFrame(1 / 60);
    const position = camera.position.clone();
    const target = controls.target.clone();
    const quaternion = camera.quaternion.clone();
    controls.clearMomentum();
    assert.deepEqual(camera.position, position);
    assert.deepEqual(controls.target, target);
    assert.deepEqual(camera.quaternion.toArray(), quaternion.toArray());
    const motion = new CameraMotion(camera.position, controls.target);
    const destination = new Vector3(50, 36, 80);
    const center = new Vector3(50, 4, 0);
    motion.moveTo(destination, center);
    for (let frame = 0; frame < 180; frame++) {
      motion.update(1 / 60);
      controls.updateFrame(1 / 60);
    }
    assert.ok(camera.position.distanceTo(destination) < 1e-10);
    assert.ok(controls.target.distanceTo(center) < 1e-10);
  } finally {
    controls.dispose();
  }
});
