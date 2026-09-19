// 稳定 ID、实例位置与拾取入口; 索引只读, 行星位置从实时实例矩阵获取.
import * as THREE from "three";
import type { GalaxySelection, Star, Planet } from "../../model/types.ts";
import type { PlanetLocation, StarSystem } from "./types.ts";

// 借用对象与场景, 不取得额外 GPU 资源; 总览仅允许拾取恒星.
export function createObjectIndex(scene: THREE.Scene, systems: readonly StarSystem[]) {
  const systemsById = new Map(systems.map((system) => [system.star.id, system]));
  const planetsById = new Map<string, PlanetLocation>();
  const picks = new Map<THREE.Object3D, { star: Star; planets?: readonly Planet[] }>();
  for (const system of systems) {
    picks.set(system.surface, { star: system.star });
    for (const shell of system.shells) {
      picks.set(shell.mesh, { star: system.star, planets: shell.planets });
      shell.planets.forEach((planet, instanceIndex) => planetsById.set(planet.id, { star: system.star, planet, mesh: shell.mesh, instanceIndex }));
    }
  }
  const starTargets = systems.map((system) => system.surface);
  // 只缓存当前星系的候选, 避免为每个星系重复存储全部恒星形成平方级引用表.
  const pickTargets: THREE.Object3D[] = [...starTargets];
  let focusedStarId: string | undefined;

  const instanceMatrix = new THREE.Matrix4();
  // 通过稳定标识解析实体, 不依赖 Vue 或调用方持有同一个对象引用.
  function findPlanet(starId: string, planetId: string): PlanetLocation | undefined {
    const location = planetsById.get(planetId);
    return location?.star.id === starId ? location : undefined;
  }

  // 即时更新祖先矩阵后返回公转后的世界坐标, 供选中环和相机共用.
  function worldPosition(location: PlanetLocation): THREE.Vector3 {
    location.mesh.getMatrixAt(location.instanceIndex, instanceMatrix);
    return location.mesh.localToWorld(new THREE.Vector3().setFromMatrixPosition(instanceMatrix));
  }

  // 总览只拾取恒星; 聚焦后加入所属行星, 无权限的行星也不能挡住恒星的点击射线.
  function pick(raycaster: THREE.Raycaster, selection: GalaxySelection): GalaxySelection {
    scene.updateMatrixWorld(true);
    const starId = selection?.star.id;
    if (starId !== focusedStarId) {
      pickTargets.length = starTargets.length;
      const focused = starId ? systemsById.get(starId) : undefined;
      if (focused) for (const shell of focused.shells) pickTargets.push(shell.mesh);
      focusedStarId = starId;
    }
    const hit = raycaster.intersectObjects(pickTargets, false)[0];
    if (!hit) return null;
    const owner = picks.get(hit.object);
    if (!owner) return null;
    const planet = hit.instanceId === undefined ? null : owner.planets?.[hit.instanceId];
    if (planet === undefined) return null;
    return { star: owner.star, planet };
  }

  return { systemsById: systemsById as ReadonlyMap<string, StarSystem>, findPlanet, worldPosition, pick };
}
