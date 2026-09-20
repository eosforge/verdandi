// 单个全屏三角形绘制深空底色; 不创建灯光、环境贴图、时间循环或全屏后期滤镜.
import * as THREE from "three";
import { sceneConfig } from "../config.ts";
import type { ResourceScope } from "../resourceScope.ts";
import { cosmicBackgroundFragment, cosmicBackgroundVertex } from "./shaders.ts";
import { createDistantStars } from "./createDistantStars.ts";
import { renderLayers } from "../rendering/layers.ts";
import { createFullscreenTriangle } from "../rendering/fullscreenTriangle.ts";

// 只在普通场景层绘制一次; 黑洞捕获该背景, 辅助线与后续光学通道不会重复绘制天空.
export function createCosmicBackground(scene: THREE.Scene, camera: THREE.PerspectiveCamera, scope: ResourceScope): void {
  const config = sceneConfig.cosmicBackground;
  if (!config.enabled) return;
  const geometry = createFullscreenTriangle(scope);
  const material = scope.own(
    new THREE.ShaderMaterial({
      uniforms: {
        inverseProjection: { value: camera.projectionMatrixInverse },
        cameraWorld: { value: camera.matrixWorld },
        baseColor: { value: new THREE.Color(sceneConfig.background) },
        coolColor: { value: new THREE.Color(0x526d9c) },
        warmColor: { value: new THREE.Color(0x987662) },
        radiationStrength: { value: config.radiationStrength },
        grainStrength: { value: config.grainStrength },
      },
      vertexShader: cosmicBackgroundVertex,
      fragmentShader: cosmicBackgroundFragment,
      depthTest: true,
      depthWrite: false,
      toneMapped: false,
    }),
  );
  const sky = new THREE.Mesh(geometry, material);
  sky.name = "CosmicBackground";
  sky.layers.set(renderLayers.scene);
  sky.frustumCulled = false;
  sky.renderOrder = -1000;
  // 仅渲染背景, 明确禁止其进入未来可能新增的全场景射线检测.
  sky.raycast = () => {};
  scene.add(sky);
  createDistantStars(scene, scope);
}
