// 规划跨星系轨道并装配星体; 自转表和黑洞时钟交给唯一帧循环更新.
import * as THREE from "three";
import { starScaleForPlanetCount } from "../../model/presentation.ts";
import type { GalaxyData, Star } from "../../model/types.ts";
import { planPlanetOrbits } from "../../model/orbitPlanner.ts";
import { planClusterLayout } from "../../model/clusterLayout.ts";
import { createCoronaMaterial, createStarMaterial } from "../materials/star.ts";
import { createPlanetTexture } from "../materials/planet.ts";
import { createBlackHole } from "../blackHole/createBlackHole.ts";
import { createPulsar } from "../pulsar/createPulsar.ts";
import { pulsarConfig } from "../pulsar/config.ts";
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

// 视觉模型和轨道包围范围共用最终倍率; 黑洞状态使用独立比例, 不叠加普通恒星放大.
function modelScale(star: Star): number {
  if (star.status === "black-hole") return sceneConfig.blackHoleScale;
  return starScaleForPlanetCount(star.planets.length) * (star.appearance === "pulsar" ? sceneConfig.pulsarScale : sceneConfig.starScale);
}

// 只取得和登记资源, 不注册输入或启动 RAF; 缺失模型或规划失败交给调用方统一回滚.
export function createStarSystems(scene: THREE.Scene, data: GalaxyData, scope: ResourceScope, models: CelestialModels) {
  const planetSurface = models.planet?.getObjectByName("Surface");
  const planetGeometry = planetSurface instanceof THREE.Mesh ? planetSurface.geometry : undefined;
  const planetRadius = modelRadius(models.planet, "Surface");
  const starRadius = modelRadius(models.star, "Surface");
  const blackHoleRadius = modelRadius(models.blackHole, "Core") * sceneConfig.blackHoleScale;
  // 每颗行星独占径向带, 再按实际轨道范围拉开各星系, 不修改输入快照.
  const planned = planPlanetOrbits(
    data.stars.flatMap((star) =>
      star.status === "available" ? star.planets.map((planet) => ({ id: planet.id, starId: star.id, position: planet.position })) : [],
    ),
    planetRadius,
    data.stars.map((star) => ({
      id: star.id,
      radius: star.status === "available" ? (star.appearance === "pulsar" ? pulsarConfig.coreRadius : starRadius) * modelScale(star) : blackHoleRadius,
    })),
  );
  const extents = new Map(
    data.stars.map((star) => [
      star.id,
      Math.max(
        planned.extents.get(star.id) ?? 0,
        star.status === "available"
          ? (star.appearance === "pulsar" ? Math.hypot(pulsarConfig.beamLength, pulsarConfig.beamRadius) : modelRadius(models.star, "Corona")) *
              modelScale(star)
          : modelRadius(models.blackHole, "Horizon") * modelScale(star),
      ),
    ]),
  );
  const { positions, orbit: stellarOrbit } = planClusterLayout(
    data,
    data.stars.map((star) => ({ id: star.id, position: star.position, extent: extents.get(star.id) ?? 0 })),
  );
  const planetTexture = scope.own(createPlanetTexture());
  const planetMaterial = scope.own(
    new THREE.MeshStandardMaterial({ map: planetTexture, bumpMap: planetTexture, bumpScale: 0.08, roughness: 0.88, metalness: 0 }),
  );
  const blackHoles: ReturnType<typeof createBlackHole>[] = [];
  const pulsars: ReturnType<typeof createPulsar>[] = [];
  const rotatingStars = new Map<string, { model: THREE.Group; speed: number }>();
  const systems: StarSystem[] = data.stars.map((star, index) => {
    const isBlackHole = star.status === "black-hole";
    const center = positions.get(star.id);
    if (!center) throw new Error(`Missing system position: ${star.id}`);
    const group = new THREE.Group();
    group.position.fromArray(center);
    scene.add(group);
    let body: THREE.Mesh;
    if (isBlackHole) {
      if (!models.blackHole) throw new Error("Black-hole model must be loaded before creating black-hole stars");
      const blackHole = createBlackHole(models.blackHole);
      // 在星图中统一缩放整个模型, 保持盘面、阴影、拾取和局部光路的比例一致.
      blackHole.group.scale.setScalar(modelScale(star));
      body = blackHole.core;
      group.add(blackHole.group);
      blackHoles.push(blackHole);
    } else if (star.appearance === "pulsar") {
      const pulsar = createPulsar(scope);
      pulsar.group.scale.setScalar(modelScale(star));
      body = pulsar.core;
      group.add(pulsar.group);
      pulsars.push(pulsar);
      rotatingStars.set(star.id, { model: pulsar.rotor, speed: pulsarConfig.radiansPerSecond });
    } else {
      const model = models.star?.clone(true);
      const surface = model?.getObjectByName("Surface");
      const corona = model?.getObjectByName("Corona");
      if (!model || !(surface instanceof THREE.Mesh) || !(corona instanceof THREE.Mesh)) throw new Error("Star model must be loaded before creating stars");
      body = surface;
      body.material = scope.own(createStarMaterial(star.color, index * 19.7));
      corona.material = scope.own(createCoronaMaterial(star.color));
      // 表面与日冕共用模型变换, 行星和连线仍以 Star 中心为基准; 自转使用统一基础速度, 允许调用方覆盖.
      model.scale.setScalar(modelScale(star));
      rotatingStars.set(star.id, { model, speed: sceneConfig.starRotationRadiansPerSecond });
      group.add(model);
    }

    // 黑洞状态只保留星体核心; 即使传入旧实体, 也不构建行星实例、索引或选择入口.
    const shells = createPlanetShells(star, planned.orbits, planetGeometry, planetMaterial, planetRadius, scope);
    for (const shell of shells) group.add(shell.mesh);
    const bodyRadius = isBlackHole
      ? modelRadius(models.blackHole, "Horizon") * modelScale(star)
      : (star.appearance === "pulsar" ? pulsarConfig.coreRadius : starRadius) * modelScale(star);
    return { star, center, surface: body, group, shells, orbitExtent: extents.get(star.id) ?? 0, bodyRadius };
  });

  return { systems, blackHoles, pulsars, rotatingStars, positions, stellarOrbit };
}
