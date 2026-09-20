// 固定全局位置与大范围星图的缩放边界, 不创建 WebGL 或 DOM.
import assert from "node:assert/strict";
import test from "node:test";
import { PerspectiveCamera, Sphere, Vector3 } from "three";
import { createSceneFraming, fitSceneFraming } from "../src/features/galaxy/runtime/scene/sceneFraming.ts";
import { CameraMotion } from "../src/features/galaxy/runtime/cameraMotion.ts";
import { sceneConfig } from "../src/features/galaxy/runtime/config.ts";

test("framing keeps the configured global position across scene sizes and viewport changes", () => {
  const frame = createSceneFraming();
  for (const radius of [0, 10, 1200]) {
    for (const aspect of [2, 1, 0.4]) {
      const bounds = new Sphere(new Vector3(200, -50, 130), radius);
      const camera = new PerspectiveCamera(45, aspect, 0.1, 1600);
      camera.position.set(50, 80, 110);
      fitSceneFraming(frame, bounds, camera);
      assert.deepEqual(frame.position.toArray(), sceneConfig.camera.initialPosition);
      assert.deepEqual(frame.target.toArray(), radius ? bounds.center.toArray() : sceneConfig.camera.overviewTarget);
      assert.deepEqual(camera.position.toArray(), [50, 80, 110], "resizing does not move the current camera");
      assert.ok(frame.maxDistance > frame.position.distanceTo(frame.target));
      assert.ok(frame.far > frame.position.distanceTo(frame.target) + radius);
    }
  }
});

test("wheel zoom has room beyond the configured initial position even for an empty scene", () => {
  for (const radius of [0, 800]) {
    const frame = createSceneFraming();
    fitSceneFraming(frame, new Sphere(new Vector3(), radius), new PerspectiveCamera(45, 1, 0.1, 1600));
    const position = frame.position.clone();
    const target = frame.target.clone();
    const distance = position.distanceTo(target);
    const motion = new CameraMotion(position, target, frame);
    motion.zoomBy(100, target);
    motion.update(0.1);
    assert.ok(position.distanceTo(target) > distance, "zooming out must not snap toward the old maximum");
    assert.ok(position.distanceTo(target) < frame.maxDistance);
  }
});

test("moving the scene center updates the return target but never repositions the global camera", () => {
  const frame = createSceneFraming();
  const camera = new PerspectiveCamera(45, 1, 0.1, 1600);
  fitSceneFraming(frame, new Sphere(new Vector3(-400, 200, 600), 700), camera);
  assert.deepEqual(frame.target.toArray(), [-400, 200, 600]);
  assert.deepEqual(frame.position.toArray(), sceneConfig.camera.initialPosition);
  fitSceneFraming(frame, new Sphere(new Vector3(300, 0, -500), 900), camera);
  assert.deepEqual(frame.target.toArray(), [300, 0, -500]);
  assert.deepEqual(frame.position.toArray(), sceneConfig.camera.initialPosition);
});
