// 每个星系的共同轨道面与行星小倾角约束, 均为确定性的展示参数而非观测星历.
import type { Position3 } from "./types.ts";

export interface OrbitalPlane {
  /** 面内单位轴, 与 transverse 正交. */
  readonly reference: Position3;
  /** 面内正向单位轴, reference × transverse = normal. */
  readonly transverse: Position3;
  /** 公转正向法线, 单位长度; 同一星系所有行星沿此方向附近运行. */
  readonly normal: Position3;
}

// 行星相对所属星系共同面的最大倾角, 单位弧度; 候选搜索不得突破这个范围.
export const maxPlanetInclination = (3 * Math.PI) / 180;

// 稳定 ID 的无符号种子, 与数组顺序、世界位置或帧时间无关.
export function orbitSeed(id: string): number {
  let seed = 2166136261;
  for (const character of id) seed = Math.imul(seed ^ character.charCodeAt(0), 16777619);
  return seed >>> 0;
}

// 为每个 Star 生成独立正交面, 相对世界 XZ 面倾斜 12..32 度, 便于总览辨认各个星系.
export function createSystemPlane(starId: string): OrbitalPlane {
  const seed = orbitSeed(starId);
  const longitude = (seed / 0x100000000) * Math.PI * 2;
  const inclination = ((12 + (((seed >>> 8) & 255) / 255) * 20) * Math.PI) / 180;
  const c = Math.cos(longitude);
  const s = Math.sin(longitude);
  const ci = Math.cos(inclination);
  const si = Math.sin(inclination);
  return { reference: [c, 0, s], transverse: [ci * s, si, -ci * c], normal: [-s * si, ci, c * si] };
}
