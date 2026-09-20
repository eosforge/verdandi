// 场景对象装配入口; 建模、运动、拾取和装饰各有独立实现, 资源由调用方作用域统一拥有.
import * as THREE from "three";
import type { GalaxyData, GalaxySelection } from "../model/types.ts";
import type { ResourceScope } from "./resourceScope.ts";
import type { CelestialModels } from "./celestialAssets.ts";
import { createStarSystems } from "./objects/createStarSystems.ts";
import { createSceneDecorations } from "./objects/createSceneDecorations.ts";
import { createObjectIndex } from "./objects/createObjectIndex.ts";
import { createObjectMotion } from "./objects/createObjectMotion.ts";
import { createStellarOrbitLines } from "./objects/createStellarOrbitLines.ts";

// 不注册事件或启动帧循环; 初始化失败由场景入口统一释放已登记资源.
export function createGalaxyObjects(scene: THREE.Scene, data: GalaxyData, scope: ResourceScope, models: CelestialModels) {
  const systemObjects = createStarSystems(scene, data, scope, models);
  const { blackHoles } = systemObjects;
  const { selectionRing, linkMaterial, updateLinks } = createSceneDecorations(scene, data, systemObjects.positions, scope);
  const stellarOrbitLines = createStellarOrbitLines(scene, systemObjects.stellarOrbit, scope);
  const { systemsById, findPlanet, worldPosition, pick } = createObjectIndex(scene, systemObjects.systems);
  const { update: updateMotion, setStarRotationSpeed } = createObjectMotion(systemObjects);
  // 先更新父节点位置再同步连线, 所有显示与拾取读取同一帧的中心.
  function update(deltaSeconds: number, selection: GalaxySelection): void {
    if (updateMotion(deltaSeconds, selection)) updateLinks();
  }
  const box = new THREE.Box3();
  for (const system of systemObjects.systems) {
    box.expandByPoint(new THREE.Vector3(...system.center).addScalar(system.orbitExtent));
    box.expandByPoint(new THREE.Vector3(...system.center).subScalar(system.orbitExtent));
  }
  const bounds = box.isEmpty() ? new THREE.Sphere(new THREE.Vector3(), 0) : box.getBoundingSphere(new THREE.Sphere());
  // 明确指定中心时, 围绕它重新包围全部星系; 总览与旋转共用此目标, 不只把节点挪到画面中间.
  const centerId = systemObjects.stellarOrbit?.centerId ?? data.centerStarId;
  if (centerId !== undefined) {
    const center = systemsById.get(centerId);
    if (!center) throw new Error("Missing overview center Star");
    bounds.center.set(...center.center);
    bounds.radius = 0;
    const position = new THREE.Vector3();
    for (const system of systemObjects.systems)
      bounds.radius = Math.max(bounds.radius, position.set(...system.center).distanceTo(bounds.center) + system.orbitExtent);
    bounds.radius = Math.max(bounds.radius, systemObjects.stellarOrbit?.extent ?? 0);
  }
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
    bounds,
    systemsById,
    horizons: blackHoles.map((blackHole) => blackHole.horizon),
    selectionRing,
    overlays: stellarOrbitLines ? [selectionRing, stellarOrbitLines] : [selectionRing],
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
