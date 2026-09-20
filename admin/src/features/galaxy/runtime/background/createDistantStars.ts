// 固定天球方向的稀疏远景星点; 无平移视差、无随机闪烁, 不参与光照或实体拾取.
import * as THREE from "three";
import { sceneConfig } from "../config.ts";
import type { ResourceScope } from "../resourceScope.ts";
import { renderLayers } from "../rendering/layers.ts";
import { distantStarVertex, distantStarFragment } from "./distantStarShaders.ts";

// 单个 Points 批次, 固定种子保证重建后星空分布不跳变; 所有资源归场景作用域.
export function createDistantStars(scene: THREE.Scene, scope: ResourceScope): void {
  const config = sceneConfig.cosmicBackground;
  const positions = new Float32Array(config.distantStarCount * 3);
  const details = new Float32Array(config.distantStarCount * 2);
  let seed = 0x73ac91;
  function random(): number {
    seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
    return seed / 0x100000000;
  }
  for (let index = 0; index < config.distantStarCount; index++) {
    const y = random() * 2 - 1;
    const angle = random() * Math.PI * 2;
    const radius = Math.sqrt(1 - y * y);
    positions.set([radius * Math.cos(angle), y, radius * Math.sin(angle)], index * 3);
    const brightness = random() ** 3;
    details.set([1.4 + brightness * 1.2, 0.2 + brightness * 0.8], index * 2);
  }
  const geometry = scope.own(new THREE.BufferGeometry());
  geometry.setAttribute("position", new THREE.BufferAttribute(positions, 3));
  geometry.setAttribute("detail", new THREE.BufferAttribute(details, 2));
  const pixelRatio = { value: 1 };
  const material = scope.own(
    new THREE.ShaderMaterial({
      uniforms: {
        pixelRatio,
        strength: { value: config.distantStarStrength },
        color: { value: new THREE.Color(0xb7c8e2) },
      },
      vertexShader: distantStarVertex,
      fragmentShader: distantStarFragment,
      transparent: true,
      blending: THREE.AdditiveBlending,
      depthTest: true,
      depthWrite: false,
      toneMapped: false,
    }),
  );
  const stars = new THREE.Points(geometry, material);
  stars.name = "DistantStars";
  stars.layers.set(renderLayers.scene);
  stars.frustumCulled = false;
  stars.renderOrder = -900;
  stars.raycast = () => {};
  stars.onBeforeRender = (renderer) => {
    pixelRatio.value = renderer.getPixelRatio();
  };
  scene.add(stars);
}
