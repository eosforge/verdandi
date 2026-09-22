// 为每颗行星分配独占径向轨道带; 静态间距覆盖任意相位, 不依赖共振周期或时序避让.
import { createPlanetOrbit, type PlanetOrbit } from "./orbit.ts";
import type { Position3 } from "./types.ts";
import { createSystemPlane, orbitSeed } from "./orbitalPlane.ts";

export interface OrbitRequest {
  readonly id: string;
  /** 所属恒星, 用于共享轨道面和径向空间预算. */
  readonly starId: string;
  /** 初始布局提示, 模长仅作为半长轴的下限参考. */
  readonly position: Position3;
}

export interface OrbitSystem {
  readonly id: string;
  /** 恒星实体包围半径, 已包含数量缩放, 单位为世界长度. */
  readonly radius: number;
}

export const orbitSpacing = {
  // 两颗行星最小表面间隔, 同时用于恒星与首颗行星之间, 单位为世界长度.
  surfaceGap: 0.12,
  // 径向摆幅 a*e 的世界长度范围; 外侧限制摆幅, 避免轨道按固定离心率指数膨胀.
  minExcursion: 0.15,
  maxExcursion: 0.35,
} as const;

// 按布局距离和 ID 排序; q_outer - Q_inner >= 2R + gap, 任意相位和小倾角下都保持分离.
export function planPlanetOrbits(requests: readonly OrbitRequest[], planetRadius: number, systems: readonly OrbitSystem[]) {
  if (!Number.isFinite(planetRadius) || planetRadius < 0) throw new RangeError("Invalid planet radius");
  const ordered = [...requests].sort((a, b) => Math.hypot(...a.position) - Math.hypot(...b.position) || (a.id < b.id ? -1 : a.id > b.id ? 1 : 0));
  const orbits = new Map<string, PlanetOrbit>();
  const extents = new Map<string, number>();
  const bands = new Map(
    systems.map((system) => {
      if (!Number.isFinite(system.radius) || system.radius < 0) throw new RangeError("Invalid stellar radius");
      extents.set(system.id, system.radius);
      // 元组固定键和值的对应关系, band 是此规划调用独占的可变进度, 不做深层只读推断.
      const band = { plane: createSystemPlane(system.id), nextPeriapsis: system.radius + planetRadius + orbitSpacing.surfaceGap };
      return [system.id, band] as const;
    }),
  );
  for (const request of ordered) {
    const band = bands.get(request.starId);
    if (!band) throw new Error("Missing orbital system: " + request.starId);
    const hintRadius = Math.hypot(...request.position);
    const position: Position3 = hintRadius > 1e-8 ? request.position : band.plane.reference;
    const shape = createPlanetOrbit(request.id, position, band.plane);
    const variation = ((orbitSeed(request.id) >>> 8) & 255) / 255;
    const excursion = orbitSpacing.minExcursion + variation * (orbitSpacing.maxExcursion - orbitSpacing.minExcursion);
    const semiMajorAxis = Math.max(hintRadius, band.nextPeriapsis + excursion);
    const eccentricity = Math.min(shape.eccentricity, excursion / semiMajorAxis);
    // 独立按 Kepler 关系计算角速度, 不量化成相同的周期档位.
    const orbit: PlanetOrbit = { ...shape, semiMajorAxis, eccentricity, meanMotion: 0.028 * (20 / semiMajorAxis) ** 1.5 };
    if (!Number.isFinite(semiMajorAxis) || !Number.isFinite(orbit.meanMotion) || orbit.meanMotion <= 0) throw new RangeError("Invalid orbital extent");
    const apoapsis = semiMajorAxis * (1 + eccentricity);
    band.nextPeriapsis = apoapsis + planetRadius * 2 + orbitSpacing.surfaceGap;
    orbits.set(request.id, orbit);
    extents.set(request.starId, apoapsis + planetRadius);
  }
  return { orbits, extents };
}
