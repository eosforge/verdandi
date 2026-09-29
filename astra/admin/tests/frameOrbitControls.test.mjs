import assert from "node:assert/strict";
import test from "node:test";
import { MOUSE, PerspectiveCamera, Vector3 } from "three";
import { CameraMotion } from "../src/features/galaxy/runtime/cameraMotion.ts";
import { FrameOrbitControls } from "../src/features/galaxy/runtime/frameOrbitControls.ts";
import { bindCanvasInput } from "../src/features/galaxy/runtime/canvasInput.ts";
import { ResourceScope } from "../src/features/galaxy/runtime/resourceScope.ts";

function createControls() {
  const document = new EventTarget();
  const canvas = new EventTarget();
  const captured = new Set();
  Object.assign(canvas, {
    ownerDocument: document,
    getRootNode: () => document,
    style: {},
    clientWidth: 800,
    clientHeight: 600,
    setPointerCapture: (id) => captured.add(id),
    releasePointerCapture: (id) => captured.delete(id),
    focus: () => {},
    getBoundingClientRect: () => ({ left: 0, top: 0, width: 800, height: 600 }),
  });
  const camera = new PerspectiveCamera(45, 800 / 600, 0.1, 1600);
  camera.position.set(0, 40, 90);
  const controls = new FrameOrbitControls(camera, canvas);
  controls.enableDamping = true;
  controls.updateFrame(0);
  camera.updateMatrixWorld();
  return { camera, controls, canvas, document, captured };
}

test("middle pointer drag pans near and expanded views without zoom, rotation or picking", () => {
  for (const distance of [100, 2500]) {
    const { camera, controls, canvas, document, captured } = createControls();
    const scope = new ResourceScope();
    try {
      controls.mouseButtons.MIDDLE = MOUSE.PAN;
      controls.maxDistance = distance * 2;
      controls.target.set(400, 20, -180);
      camera.position.copy(controls.target).add(new Vector3(0, 0.4, 0.9).normalize().multiplyScalar(distance));
      controls.updateFrame(0);
      camera.updateMatrixWorld();
      const motion = new CameraMotion(camera.position, controls.target);
      let picks = 0;
      bindCanvasInput({ canvas, camera, target: controls.target, motion, pick: () => picks++, overview: () => {} }, scope);
      function send(surface, type, x, y) {
        const event = new Event(type, { cancelable: true });
        Object.assign(event, { pointerId: 1, pointerType: "mouse", isPrimary: true, button: 1, buttons: type === "pointerup" ? 0 : 4, clientX: x, clientY: y });
        surface.dispatchEvent(event);
        return event;
      }
      const position = camera.position.clone();
      const target = controls.target.clone();
      const orientation = camera.quaternion.clone();
      assert.equal(send(canvas, "pointerdown", 400, 300).defaultPrevented, true);
      assert.ok(captured.has(1), "default prevention must still allow OrbitControls pointer capture");
      send(document, "pointermove", 460, 325);
      assert.deepEqual(camera.position, position, "pointer events only queue movement");
      for (let frame = 0; frame < 60; frame++) {
        motion.update(1 / 60);
        controls.updateFrame(1 / 60);
        camera.updateMatrixWorld();
      }
      const cameraShift = camera.position.clone().sub(position);
      const targetShift = controls.target.clone().sub(target);
      assert.ok(cameraShift.length() > 1);
      assert.ok(cameraShift.distanceTo(targetShift) < 1e-9, "pan translates camera and target equally");
      assert.ok(Math.abs(camera.position.distanceTo(controls.target) - distance) < 1e-9);
      assert.ok(camera.quaternion.angleTo(orientation) < 1e-7);
      assert.equal(picks, 0);
      send(document, "pointerup", 460, 325);
      assert.equal(captured.size, 0);
    } finally {
      scope.dispose();
      controls.dispose();
    }
  }
});

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
