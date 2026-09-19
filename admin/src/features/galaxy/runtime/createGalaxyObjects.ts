// 场景对象装配入口; 建模、运动、拾取和装饰各有独立实现, 资源由调用方作用域统一拥有.
import type * as THREE from "three";
import type { GalaxyData, GalaxySelection } from "../model/types.ts";
import type { ResourceScope } from "./resourceScope.ts";
import type { CelestialModels } from "./celestialAssets.ts";
import { createStarSystems } from "./objects/createStarSystems.ts";
import { createSceneDecorations } from "./objects/createSceneDecorations.ts";
import { createObjectIndex } from "./objects/createObjectIndex.ts";
import { createObjectMotion } from "./objects/createObjectMotion.ts";

// 不注册事件或启动帧循环; 初始化失败由场景入口统一释放已登记资源.
export function createGalaxyObjects(scene: THREE.Scene, data: GalaxyData, scope: ResourceScope, models: CelestialModels) {
  const systemObjects = createStarSystems(scene, data, scope, models);
  const { blackHoles } = systemObjects;
  const { selectionRing, linkMaterial } = createSceneDecorations(scene, data, scope);
  const { systemsById, findPlanet, worldPosition, pick } = createObjectIndex(scene, systemObjects.systems);
  const { update, setStarRotationSpeed } = createObjectMotion(systemObjects);
  // 仅行星选中环朝向相机; 黑洞完全使用模型的世界姿态, 不跟随相机翻转.
  function faceCamera(quaternion: THREE.Quaternion): void {
    selectionRing.quaternion.copy(quaternion);
  }

  // 根据唯一选择状态更新装饰, 不在显示对象层维护第二份业务选择.
  function showSelection(selection: GalaxySelection): void {
    selectionRing.visible = false;
    linkMaterial.opacity = selection ? 0.12 : 0.3;
    if (selection?.planet) {
      const location = findPlanet(selection.star.id, selection.planet.id);
      if (location) {
        selectionRing.position.copy(worldPosition(location));
        selectionRing.visible = true;
      }
    }
  }

  return {
    systemsById,
    horizons: blackHoles.map((blackHole) => blackHole.horizon),
    selectionRing,
    update,
    setStarRotationSpeed,
    faceCamera,
    findPlanet,
    worldPosition,
    pick,
    showSelection,
  };
}

export type GalaxyObjects = ReturnType<typeof createGalaxyObjects>;
