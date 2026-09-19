// 无 GPU 回归只验证通道隔离与资源恢复, 不替代着色器和浏览器画面验收.
import assert from "node:assert/strict";
import test from "node:test";
import * as THREE from "three";
import { createLensingRenderer } from "../src/features/galaxy/runtime/lensingRenderer.ts";
import { ResourceScope } from "../src/features/galaxy/runtime/resourceScope.ts";

test("lensing isolates scene, optical volumes and overlays, restores state and resizes shared capture", () => {
  const scope = new ResourceScope();
  const scene = new THREE.Scene();
  const camera = new THREE.PerspectiveCamera();
  camera.layers.enable(3);
  const originalMask = camera.layers.mask;
  const material = scope.own(new THREE.ShaderMaterial());
  const horizon = new THREE.Mesh(scope.own(new THREE.SphereGeometry()), material);
  const overlay = new THREE.Object3D();
  let activeTarget = null;
  let width = 800;
  let fail = false;
  const calls = [];
  const renderer = {
    autoClear: false,
    getDrawingBufferSize: (size) => size.set(width, 600),
    getRenderTarget: () => activeTarget,
    setRenderTarget: (target) => (activeTarget = target),
    render(renderScene, renderCamera) {
      calls.push({ renderScene, mask: renderCamera.layers.mask, target: activeTarget, clear: this.autoClear });
      if (fail && renderScene === scene && renderCamera.layers.mask === 2) throw new Error("optics failed");
    },
  };
  const draw = createLensingRenderer(renderer, scene, camera, [horizon], overlay, scope);
  draw();
  assert.equal(calls.length, 4);
  const capture = calls[0].target;
  assert.ok(capture instanceof THREE.WebGLRenderTarget);
  assert.equal(capture.width, 800);
  assert.equal(capture.height, 600);
  assert.equal(material.uniforms.backgroundColor.value, capture.texture);
  assert.equal(material.uniforms.backgroundDepth.value, capture.depthTexture);
  assert.deepEqual(
    calls.filter((call) => call.renderScene === scene).map((call) => call.mask),
    [1, 2, 4],
  );
  assert.deepEqual(
    calls.map((call) => call.clear),
    [true, true, false, false],
  );
  assert.equal(activeTarget, null);
  assert.equal(camera.layers.mask, originalMask);
  assert.equal(renderer.autoClear, false);
  width = 1200;
  overlay.visible = false;
  calls.length = 0;
  draw();
  assert.equal(capture.width, 1200);
  assert.equal(calls.length, 3);
  fail = true;
  assert.throws(draw, /optics failed/);
  assert.equal(activeTarget, null);
  assert.equal(camera.layers.mask, originalMask);
  assert.equal(renderer.autoClear, false);
  assert.deepEqual(scope.dispose(), []);
  assert.equal(horizon.layers.mask, 1);
  assert.equal(overlay.layers.mask, 1);
  assert.equal(material.uniforms.backgroundEnabled.value, 0);
  assert.deepEqual(scope.dispose(), []);
});

test("a scene without black holes needs no capture or extra render passes", () => {
  const scope = new ResourceScope();
  const scene = new THREE.Scene();
  const camera = new THREE.PerspectiveCamera();
  let calls = 0;
  const renderer = {
    render(actualScene, actualCamera) {
      assert.equal(actualScene, scene);
      assert.equal(actualCamera, camera);
      calls++;
    },
  };
  createLensingRenderer(renderer, scene, camera, [], new THREE.Object3D(), scope)();
  assert.equal(calls, 1);
  assert.deepEqual(scope.dispose(), []);
});
