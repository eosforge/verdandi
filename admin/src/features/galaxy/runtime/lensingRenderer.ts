// 场景颜色与深度只捕获一次, 黑洞共享只读背景; 不递归折射其它黑洞或 UI 装饰.
import * as THREE from "three";
import type { ResourceScope } from "./resourceScope.ts";

// 返回与主循环共用的绘制入口; 没有黑洞时保持原始单次绘制路径.
export function createLensingRenderer(
  renderer: THREE.WebGLRenderer,
  scene: THREE.Scene,
  camera: THREE.PerspectiveCamera,
  horizons: readonly THREE.Mesh[],
  overlay: THREE.Object3D,
  scope: ResourceScope,
): () => void {
  if (!horizons.length) return () => renderer.render(scene, camera);
  const target = scope.own(new THREE.WebGLRenderTarget(1, 1, { depthBuffer: true, samples: 4 }));
  target.depthTexture = new THREE.DepthTexture(1, 1, THREE.UnsignedIntType);
  const size = new THREE.Vector2();
  const materials = new Set<THREE.ShaderMaterial>();
  for (const horizon of horizons) {
    const mask = horizon.layers.mask;
    scope.defer(() => (horizon.layers.mask = mask));
    horizon.layers.set(1);
    if (horizon.material instanceof THREE.ShaderMaterial) materials.add(horizon.material);
  }
  const overlayMask = overlay.layers.mask;
  scope.defer(() => (overlay.layers.mask = overlayMask));
  overlay.layers.set(2);
  for (const material of materials) {
    Object.assign(material.uniforms, {
      backgroundColor: { value: target.texture },
      backgroundDepth: { value: target.depthTexture },
      backgroundSize: { value: size },
      backgroundEnabled: { value: 1 },
      cameraRange: { value: new THREE.Vector2(camera.near, camera.far) },
    });
    scope.defer(() => {
      const enabled = material.uniforms.backgroundEnabled;
      if (enabled) enabled.value = 0;
    });
  }
  const copyMaterial = scope.own(
    new THREE.ShaderMaterial({
      uniforms: { source: { value: target.texture }, depth: { value: target.depthTexture } },
      vertexShader: `
        varying vec2 screenUv;
        void main() {
          screenUv = position.xy * 0.5 + 0.5;
          gl_Position = vec4(position.xy, 0.0, 1.0);
        }
      `,
      fragmentShader: `
        uniform sampler2D source;
        uniform sampler2D depth;
        varying vec2 screenUv;
        void main() {
          gl_FragColor = texture2D(source, screenUv);
          gl_FragDepth = texture2D(depth, screenUv).r;
          #include <colorspace_fragment>
        }
      `,
      depthFunc: THREE.AlwaysDepth,
      depthWrite: true,
      toneMapped: false,
    }),
  );
  // 全屏三角形复制颜色和深度, 保持前景星体对黑洞的正常遮挡.
  const geometry = scope.own(new THREE.BufferGeometry());
  geometry.setAttribute("position", new THREE.Float32BufferAttribute([-1, -1, 0, 3, -1, 0, -1, 3, 0], 3));
  const copyScene = new THREE.Scene();
  copyScene.add(new THREE.Mesh(geometry, copyMaterial));
  scope.defer(() => copyScene.clear());
  const copyCamera = new THREE.Camera();

  // 尺寸随实际绘图缓冲改变; finally 恢复调用方状态, 出错仍可由统一清理路径回收.
  return () => {
    renderer.getDrawingBufferSize(size);
    if (target.width !== size.x || target.height !== size.y) target.setSize(size.x, size.y);
    const previousTarget = renderer.getRenderTarget();
    const previousLayers = camera.layers.mask;
    const previousClear = renderer.autoClear;
    try {
      renderer.autoClear = true;
      camera.layers.set(0);
      renderer.setRenderTarget(target);
      renderer.render(scene, camera);
      renderer.setRenderTarget(previousTarget);
      renderer.render(copyScene, copyCamera);
      renderer.autoClear = false;
      camera.layers.set(1);
      renderer.render(scene, camera);
      if (overlay.visible) {
        camera.layers.set(2);
        renderer.render(scene, camera);
      }
    } finally {
      renderer.autoClear = previousClear;
      camera.layers.mask = previousLayers;
      renderer.setRenderTarget(previousTarget);
    }
  };
}
