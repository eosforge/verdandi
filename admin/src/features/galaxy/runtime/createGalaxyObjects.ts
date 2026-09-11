import * as THREE from "three";
import { planetColors, planetKinds, starScaleForPlanetCount } from "../model/presentation.ts";
import type { GalaxyData, GalaxySelection, PeerStar, Planet } from "../model/types.ts";
import { createCoronaMaterial, createStarMaterial } from "./materials/star.ts";
import { createPlanetTexture } from "./materials/planet.ts";
import type { ResourceScope } from "./resourceScope.ts";
import { createBlackHole } from "./createBlackHole.ts";
import { sceneConfig } from "./config.ts";
import type { CelestialModels } from "./celestialAssets.ts";
import { writeOrbitPosition, type PlanetOrbit } from "../model/orbit.ts";
import { planPlanetOrbits } from "../model/orbitPlanner.ts";

interface OrbitShell {
  mesh: THREE.InstancedMesh;
  planets: readonly Planet[];
  orbits: readonly PlanetOrbit[];
}
interface StarSystem {
  peer: PeerStar;
  star: THREE.Mesh;
  group: THREE.Group;
  shells: OrbitShell[];
  orbitExtent: number;
}
interface PlanetLocation {
  peer: PeerStar;
  planet: Planet;
  mesh: THREE.InstancedMesh;
  instanceIndex: number;
}

// 包围半径覆盖绕局部原点旋转的完整模型, 包围球中心偏移也计入轨道安全距离.
function modelRadius(model: THREE.Group | undefined, name: string): number {
  const mesh = model?.getObjectByName(name);
  if (!(mesh instanceof THREE.Mesh)) return 0;
  if (!mesh.geometry.boundingSphere) mesh.geometry.computeBoundingSphere();
  const sphere = mesh.geometry.boundingSphere;
  return sphere ? sphere.radius + sphere.center.length() : 0;
}

// 构造显示对象并将所有 GPU 资源交给调用者作用域, 不注册事件或启动帧循环.
export function createGalaxyObjects(scene: THREE.Scene, data: GalaxyData, scope: ResourceScope, models: CelestialModels) {
  const planetSurface = models.planet?.getObjectByName("Surface");
  const planetGeometry = planetSurface instanceof THREE.Mesh ? planetSurface.geometry : undefined;
  if (planetGeometry && !planetGeometry.boundingSphere) planetGeometry.computeBoundingSphere();
  const planetRadius = modelRadius(models.planet, "Surface");
  const starRadius = modelRadius(models.star, "Surface");
  const blackHoleRadius = modelRadius(models.blackHole, "Core") * 0.75;
  // 所有星系与类型一起检查同步运动, 初始布局可调整, 但原始快照保持不变.
  const planned = planPlanetOrbits(
    data.peers.flatMap((peer) =>
      peer.status === "available" ? peer.planets.map((planet) => ({ id: planet.id, position: planet.position, center: peer.position })) : [],
    ),
    planetRadius,
    data.peers.map((peer) => ({
      center: peer.position,
      radius: peer.status === "available" ? starRadius * starScaleForPlanetCount(peer.planets.length) : blackHoleRadius,
    })),
  );
  const planetTexture = scope.own(createPlanetTexture());
  const planetMaterial = scope.own(
    new THREE.MeshStandardMaterial({ map: planetTexture, bumpMap: planetTexture, bumpScale: 0.08, roughness: 0.88, metalness: 0 }),
  );
  const blackHoles: ReturnType<typeof createBlackHole>[] = [];
  const rotatingStars = new Map<string, { model: THREE.Group; speed: number }>();
  scene.add(new THREE.AmbientLight(0xbad2ff, 2));
  const keyLight = new THREE.DirectionalLight(0xffffff, 3);
  keyLight.position.set(-40, 100, 80);
  scene.add(keyLight);

  const systems: StarSystem[] = data.peers.map((peer, index) => {
    const unavailable = peer.status === "unavailable";
    const group = new THREE.Group();
    group.position.fromArray(peer.position);
    scene.add(group);
    let star: THREE.Mesh;
    if (unavailable) {
      if (!models.blackHole) throw new Error("Black-hole model must be loaded before creating unavailable peers");
      const blackHole = createBlackHole(models.blackHole);
      // 在星图中统一缩放整个模型, 保持盘面、阴影、拾取和局部光路的比例一致.
      blackHole.group.scale.setScalar(0.75);
      star = blackHole.core;
      group.add(blackHole.group);
      blackHoles.push(blackHole);
    } else {
      const model = models.star?.clone(true);
      const surface = model?.getObjectByName("Surface");
      const corona = model?.getObjectByName("Corona");
      if (!model || !(surface instanceof THREE.Mesh) || !(corona instanceof THREE.Mesh)) throw new Error("Star model must be loaded before creating peers");
      star = surface;
      star.material = scope.own(createStarMaterial(peer.color, index * 19.7));
      corona.material = scope.own(createCoronaMaterial(peer.color));
      // 表面与日冕共用模型变换, 行星和连线仍以 Peer 中心为基准; 自转使用统一基础速度, 允许调用方覆盖.
      model.scale.setScalar(starScaleForPlanetCount(peer.planets.length));
      rotatingStars.set(peer.id, { model, speed: sceneConfig.starRotationRadiansPerSecond });
      group.add(model);
    }

    // 不可用节点只保留核心; 即使传入旧实体, 也不构建实例、拾取索引或行星选择入口.
    const shells = (unavailable ? [] : planetKinds).flatMap((kind): OrbitShell[] => {
      const planets = peer.planets.filter((planet) => planet.kind === kind);
      if (!planets.length) return [];
      if (!planetGeometry) throw new Error("Planet model must be loaded before creating entities");
      const mesh = scope.own(new THREE.InstancedMesh(planetGeometry, planetMaterial, planets.length));
      mesh.instanceMatrix.setUsage(THREE.DynamicDrawUsage);
      const orbits = planets.map((planet) => {
        const orbit = planned.orbits.get(planet.id);
        if (!orbit) throw new Error(`Missing planned orbit: ${planet.id}`);
        return orbit;
      });
      const matrix = new THREE.Matrix4();
      const initialPosition: [number, number, number] = [0, 0, 0];
      orbits.forEach((orbit, index) => {
        matrix.makeRotationFromEuler(new THREE.Euler(index * 0.71, index * 1.37, index * 0.29));
        writeOrbitPosition(orbit, 0, initialPosition);
        matrix.setPosition(...initialPosition);
        mesh.setMatrixAt(index, matrix);
        mesh.setColorAt(index, new THREE.Color(planetColors[kind]));
      });
      mesh.instanceMatrix.needsUpdate = true;
      if (mesh.instanceColor) mesh.instanceColor.needsUpdate = true;
      // 预先包围所有椭圆的远日点, 公转后剔除和拾取仍有效, 不逐帧重算包围体.
      const orbitExtent = Math.max(...orbits.map((orbit) => orbit.semiMajorAxis * (1 + orbit.eccentricity)));
      mesh.boundingSphere = new THREE.Sphere(new THREE.Vector3(), orbitExtent + planetRadius);
      group.add(mesh);
      return [
        {
          mesh,
          planets,
          orbits,
        },
      ];
    });
    return { peer, star, group, shells, orbitExtent: Math.max(0, ...shells.map((shell) => shell.mesh.boundingSphere?.radius ?? 0)) };
  });

  const peersById = new Map(data.peers.map((peer) => [peer.id, peer]));
  const linkPositions: number[] = [];
  for (const link of data.links) {
    const source = peersById.get(link.source);
    const target = peersById.get(link.target);
    if (source?.status === "available" && target?.status === "available") {
      linkPositions.push(...source.position, ...target.position);
    }
  }
  const linkGeometry = scope.own(new THREE.BufferGeometry());
  linkGeometry.setAttribute("position", new THREE.Float32BufferAttribute(linkPositions, 3));
  const linkMaterial = scope.own(new THREE.LineBasicMaterial({ color: 0x75b9e6, transparent: true, opacity: 0.3, depthWrite: false }));
  scene.add(new THREE.LineSegments(linkGeometry, linkMaterial));

  const selectionRing = new THREE.Mesh(
    scope.own(new THREE.RingGeometry(1.2, 1.3, 48)),
    scope.own(new THREE.MeshBasicMaterial({ color: 0xffffff, side: THREE.DoubleSide, transparent: true, opacity: 0.8, depthTest: false, depthWrite: false })),
  );
  selectionRing.visible = false;
  selectionRing.renderOrder = 2;
  scene.add(selectionRing);

  const systemsById = new Map(systems.map((system) => [system.peer.id, system]));
  const planetsById = new Map<string, PlanetLocation>();
  const picks = new Map<THREE.Object3D, { peer: PeerStar; planets?: readonly Planet[] }>();
  for (const system of systems) {
    picks.set(system.star, { peer: system.peer });
    for (const shell of system.shells) {
      picks.set(shell.mesh, { peer: system.peer, planets: shell.planets });
      shell.planets.forEach((planet, instanceIndex) => planetsById.set(planet.id, { peer: system.peer, planet, mesh: shell.mesh, instanceIndex }));
    }
  }
  const starTargets = systems.map((system) => system.star);
  const orbitPosition: [number, number, number] = [0, 0, 0];
  const instanceMatrix = new THREE.Matrix4();
  let orbitalSeconds = 0;

  // 自转速度独立于数量和选择状态; 零速保留当前姿态, 负速反转, 无效输入不改变原速度.
  function setStarRotationSpeed(peerId: string, radiansPerSecond: number): boolean {
    const star = rotatingStars.get(peerId);
    if (!star || !Number.isFinite(radiansPerSecond)) return false;
    star.speed = radiansPerSecond;
    return true;
  }

  // 自转使用真实帧增量; 公转总览五倍、恒星视图两倍、行星详情暂停, 每批仅上传一次矩阵.
  function update(deltaSeconds: number, selection: GalaxySelection): void {
    if (!Number.isFinite(deltaSeconds) || deltaSeconds <= 0) return;
    for (const blackHole of blackHoles) blackHole.update(deltaSeconds);
    for (const star of rotatingStars.values()) {
      // 先按周期折叠时间, 避免有限但很大的输入在速度乘法时溢出.
      if (star.speed !== 0) star.model.rotateY((deltaSeconds % ((Math.PI * 2) / Math.abs(star.speed))) * star.speed);
    }
    if (selection?.planet) return;
    orbitalSeconds = (orbitalSeconds + deltaSeconds * (selection ? 2 : 5)) % planned.period;
    for (const system of systems) {
      for (const shell of system.shells) {
        shell.orbits.forEach((orbit, index) => {
          writeOrbitPosition(orbit, orbitalSeconds, orbitPosition);
          shell.mesh.getMatrixAt(index, instanceMatrix);
          instanceMatrix.setPosition(...orbitPosition);
          shell.mesh.setMatrixAt(index, instanceMatrix);
        });
        shell.mesh.instanceMatrix.needsUpdate = true;
      }
    }
  }

  // 仅行星选中环朝向相机; 黑洞完全使用模型的世界姿态, 不跟随相机翻转.
  function faceCamera(quaternion: THREE.Quaternion): void {
    selectionRing.quaternion.copy(quaternion);
  }

  // 通过稳定标识解析实体, 不依赖 Vue 或调用方持有同一个对象引用.
  function findPlanet(peerId: string, planetId: string): PlanetLocation | undefined {
    const location = planetsById.get(planetId);
    return location?.peer.id === peerId ? location : undefined;
  }

  // 即时更新祖先矩阵后返回公转后的世界坐标, 供选中环和相机共用.
  function worldPosition(location: PlanetLocation): THREE.Vector3 {
    location.mesh.getMatrixAt(location.instanceIndex, instanceMatrix);
    return location.mesh.localToWorld(new THREE.Vector3().setFromMatrixPosition(instanceMatrix));
  }

  // 总览只拾取恒星; 聚焦后加入所属行星, 无权限的行星也不能挡住恒星的点击射线.
  function pick(raycaster: THREE.Raycaster, selection: GalaxySelection): GalaxySelection {
    scene.updateMatrixWorld(true);
    const focusedSystem = selection ? systemsById.get(selection.peer.id) : undefined;
    const pickTargets: THREE.Object3D[] = focusedSystem ? [...starTargets, ...focusedSystem.shells.map((shell) => shell.mesh)] : starTargets;
    const hit = raycaster.intersectObjects(pickTargets, false)[0];
    if (!hit) return null;
    const owner = picks.get(hit.object);
    if (!owner) return null;
    const planet = hit.instanceId === undefined ? null : owner.planets?.[hit.instanceId];
    if (planet === undefined) return null;
    return { peer: owner.peer, planet };
  }

  // 根据唯一选择状态更新装饰, 不在显示对象层维护第二份业务选择.
  function showSelection(selection: GalaxySelection): void {
    selectionRing.visible = false;
    linkMaterial.opacity = selection ? 0.12 : 0.3;
    if (selection?.planet) {
      const location = findPlanet(selection.peer.id, selection.planet.id);
      if (location) {
        selectionRing.position.copy(worldPosition(location));
        selectionRing.visible = true;
      }
    }
  }

  return { systemsById, selectionRing, update, setStarRotationSpeed, faceCamera, findPlanet, worldPosition, pick, showSelection };
}
