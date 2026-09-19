// 场景内部索引契约; 只读关联关系, Three.js 实例仍由所属运行时更新.
import type * as THREE from "three";
import type { Star, Planet } from "../../model/types.ts";
import type { PlanetOrbit } from "../../model/orbit.ts";

export interface OrbitShell {
  readonly mesh: THREE.InstancedMesh;
  readonly planets: readonly Planet[];
  readonly orbits: readonly PlanetOrbit[];
}
export interface StarSystem {
  readonly star: Star;
  readonly surface: THREE.Mesh;
  readonly group: THREE.Group;
  readonly shells: readonly OrbitShell[];
  readonly orbitExtent: number;
}
export interface PlanetLocation {
  readonly star: Star;
  readonly planet: Planet;
  readonly mesh: THREE.InstancedMesh;
  readonly instanceIndex: number;
}
