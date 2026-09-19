// 规划跨星系轨道并装配星体; 自转表和黑洞时钟交给唯一帧循环更新.
import * as THREE from "three";
import { starScaleForPlanetCount } from "../../model/presentation.ts";
import type { GalaxyData } from "../../model/types.ts";
import { planPlanetOrbits } from "../../model/orbitPlanner.ts";
import { createCoronaMaterial, createStarMaterial } from "../materials/star.ts";
import { createPlanetTexture } from "../materials/planet.ts";
import { createBlackHole } from "../blackHole/createBlackHole.ts";
import { sceneConfig } from "../config.ts";
import type { ResourceScope } from "../resourceScope.ts";
import type { CelestialModels } from "../celestialAssets.ts";
import { createPlanetShells } from "./createPlanetShells.ts";
import type { StarSystem } from "./types.ts";

// 包围半径覆盖绕局部原点旋转的完整模型, 包围球中心偏移也计入轨道安全距离.
function modelRadius(model: THREE.Group | undefined, name: string): number {
  const mesh = model?.getObjectByName(name);
  if (!(mesh instanceof THREE.Mesh)) return 0;
  if (!mesh.geometry.boundingSphere) mesh.geometry.computeBoundingSphere();
  const sphere = mesh.geometry.boundingSphere;
  return sphere ? sphere.radius + sphere.center.length() : 0;
}

// 只取得和登记资源, 不注册输入或启动 RAF; 缺失模型或规划失败交给调用方统一回滚.
export function createStarSystems(scene: THREE.Scene, data: GalaxyData, scope: ResourceScope, models: CelestialModels) {
  const planetSurface = models.planet?.getObjectByName("Surface");
  const planetGeometry = planetSurface instanceof THREE.Mesh ? planetSurface.geometry : undefined;
  const planetRadius = modelRadius(models.planet, "Surface");
  const starRadius = modelRadius(models.star, "Surface");
  const blackHoleRadius = modelRadius(models.blackHole, "Core") * sceneConfig.blackHoleScale;
  // 所有星系与类型一起检查同步运动, 初始布局可调整, 但原始快照保持不变.
  const planned = planPlanetOrbits(
    data.stars.flatMap((star) =>
      star.status === "available" ? star.planets.map((planet) => ({ id: planet.id, position: planet.position, center: star.position })) : [],
    ),
    planetRadius,
    data.stars.map((star) => ({
      center: star.position,
      radius: star.status === "available" ? starRadius * starScaleForPlanetCount(star.planets.length) : blackHoleRadius,
    })),
  );
  const planetTexture = scope.own(createPlanetTexture());
  const planetMaterial = scope.own(
    new THREE.MeshStandardMaterial({ map: planetTexture, bumpMap: planetTexture, bumpScale: 0.08, roughness: 0.88, metalness: 0 }),
  );
  const blackHoles: ReturnType<typeof createBlackHole>[] = [];
  const rotatingStars = new Map<string, { model: THREE.Group; speed: number }>();
  const systems: StarSystem[] = data.stars.map((star, index) => {
    const unavailable = star.status === "unavailable";
    const group = new THREE.Group();
    group.position.fromArray(star.position);
    scene.add(group);
    let body: THREE.Mesh;
    if (unavailable) {
      if (!models.blackHole) throw new Error("Black-hole model must be loaded before creating unavailable stars");
      const blackHole = createBlackHole(models.blackHole);
      // 在星图中统一缩放整个模型, 保持盘面、阴影、拾取和局部光路的比例一致.
      blackHole.group.scale.setScalar(sceneConfig.blackHoleScale);
      body = blackHole.core;
      group.add(blackHole.group);
      blackHoles.push(blackHole);
    } else {
      const model = models.star?.clone(true);
      const surface = model?.getObjectByName("Surface");
      const corona = model?.getObjectByName("Corona");
      if (!model || !(surface instanceof THREE.Mesh) || !(corona instanceof THREE.Mesh)) throw new Error("Star model must be loaded before creating stars");
      body = surface;
      body.material = scope.own(createStarMaterial(star.color, index * 19.7));
      corona.material = scope.own(createCoronaMaterial(star.color));
      // 表面与日冕共用模型变换, 行星和连线仍以 Star 中心为基准; 自转使用统一基础速度, 允许调用方覆盖.
      model.scale.setScalar(starScaleForPlanetCount(star.planets.length));
      rotatingStars.set(star.id, { model, speed: sceneConfig.starRotationRadiansPerSecond });
      group.add(model);
    }

    // 不可用节点只保留核心; 即使传入旧实体, 也不构建实例、拾取索引或行星选择入口.
    const shells = createPlanetShells(star, planned.orbits, planetGeometry, planetMaterial, planetRadius, scope);
    for (const shell of shells) group.add(shell.mesh);
    return { star, surface: body, group, shells, orbitExtent: Math.max(0, ...shells.map((shell) => shell.mesh.boundingSphere?.radius ?? 0)) };
  });

  return { systems, blackHoles, rotatingStars, period: planned.period };
}
