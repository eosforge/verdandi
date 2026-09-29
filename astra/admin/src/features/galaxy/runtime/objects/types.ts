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
  /** 公转时原位更新的展示中心, 与连线共用; 不覆盖 Star.position 的输入提示. */
  readonly center: [number, number, number];
  readonly surface: THREE.Mesh;
  readonly group: THREE.Group;
  readonly shells: readonly OrbitShell[];
  readonly orbitExtent: number;
  /** 本体世界半径; 黑洞包含完整光学体积, 用作点击范围基准, 不含卫星或脉冲光束. */
  readonly bodyRadius: number;
}
export interface PlanetLocation {
  readonly star: Star;
  readonly planet: Planet;
  readonly mesh: THREE.InstancedMesh;
  readonly instanceIndex: number;
}
