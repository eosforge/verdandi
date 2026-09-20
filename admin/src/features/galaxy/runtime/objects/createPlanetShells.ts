// 按实体类型建立实例批次; 几何与材质借用场景资源, 实例缓冲登记到作用域.
import * as THREE from "three";
import { planetColors, planetKinds } from "../../model/presentation.ts";
import { writeOrbitPosition, type PlanetOrbit } from "../../model/orbit.ts";
import type { Star } from "../../model/types.ts";
import type { ResourceScope } from "../resourceScope.ts";
import type { OrbitShell } from "./types.ts";

// 无行星时返回空批次; 每条轨道必须已完成全局防碰撞规划, 此处不重新生成.
export function createPlanetShells(
  star: Star,
  planned: ReadonlyMap<string, PlanetOrbit>,
  planetGeometry: THREE.BufferGeometry | undefined,
  planetMaterial: THREE.Material,
  planetRadius: number,
  scope: ResourceScope,
): OrbitShell[] {
  return (star.status === "black-hole" ? [] : planetKinds).flatMap((kind): OrbitShell[] => {
    const planets = star.planets.filter((planet) => planet.kind === kind);
    if (!planets.length) return [];
    if (!planetGeometry) throw new Error("Planet model must be loaded before creating entities");
    const mesh = scope.own(new THREE.InstancedMesh(planetGeometry, planetMaterial, planets.length));
    mesh.instanceMatrix.setUsage(THREE.DynamicDrawUsage);
    const orbits = planets.map((planet) => {
      const orbit = planned.get(planet.id);
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
    return [
      {
        mesh,
        planets,
        orbits,
      },
    ];
  });
}
