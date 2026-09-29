// 扩大恒星点击容差, 仅在本体和可选行星没有精确命中时使用.
import * as THREE from "three";
import type { GalaxySelection, Star } from "../../model/types.ts";
import { sceneConfig } from "../config.ts";
import type { StarSystem } from "./types.ts";

// 借用星系对象, 不新增渲染资源; 调用前必须已更新场景世界矩阵.
export function createStarPicker(systems: readonly StarSystem[]): (raycaster: THREE.Raycaster) => GalaxySelection {
  // 普通恒星保存 Surface 包围球; 黑洞使用含吸积盘的完整光学范围, 脉冲星保留本体拾取.
  const regions = systems
    .filter((system) => system.star.status === "black-hole" || system.star.appearance !== "pulsar")
    .map((system) => {
      const geometry = system.surface.geometry;
      if (!geometry.boundingSphere) geometry.computeBoundingSphere();
      const sphere = geometry.boundingSphere;
      if (!sphere) throw new Error(`Missing stellar bounds: ${system.star.id}`);
      return { system, sphere: sphere.clone() };
    });
  const worldRegion = new THREE.Sphere();
  const regionOffset = new THREE.Vector3();

  // 局部包围球随核心世界矩阵变换, 同步数量缩放、自转和公转, 不新增 GPU 资源.
  // 比较射线有效区间的首次进入距离; 支持镜头已经位于区域内部的情况.
  function pickRegion(raycaster: THREE.Raycaster): GalaxySelection {
    let nearest: Star | undefined;
    let nearestDistance = Infinity;
    for (const { system, sphere } of regions) {
      if (system.star.status === "black-hole") {
        // bodyRadius 已计入黑洞最终世界缩放, 不再乘 Core 的缩放以免重复放大.
        worldRegion.center.set(...system.center);
        worldRegion.radius = system.bodyRadius;
      } else worldRegion.copy(sphere).applyMatrix4(system.surface.matrixWorld);
      worldRegion.radius *= sceneConfig.starPickDiameterScale;
      regionOffset.copy(worldRegion.center).sub(raycaster.ray.origin);
      const along = regionOffset.dot(raycaster.ray.direction);
      const perpendicularSquared = Math.max(0, regionOffset.lengthSq() - along * along);
      const radiusSquared = worldRegion.radius * worldRegion.radius;
      if (perpendicularSquared > radiusSquared) continue;
      const halfChord = Math.sqrt(radiusSquared - perpendicularSquared);
      const entry = Math.max(0, raycaster.near, along - halfChord);
      const exit = Math.min(raycaster.far, along + halfChord);
      if (entry > exit) continue;
      if (entry < nearestDistance || (entry === nearestDistance && (!nearest || system.star.id < nearest.id))) {
        nearest = system.star;
        nearestDistance = entry;
      }
    }
    return nearest ? { star: nearest, planet: null } : null;
  }

  return pickRegion;
}
