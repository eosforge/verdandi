// 焦点椭圆与 Kepler 时间求解; 每颗行星相对所属星系的共同面仅允许小倾角.
import type { Position3 } from "./types.ts";
import { maxPlanetInclination, orbitSeed, type OrbitalPlane } from "./orbitalPlane.ts";

export interface PlanetOrbit {
  readonly semiMajorAxis: number;
  readonly eccentricity: number;
  readonly periapsis: Position3;
  readonly transverse: Position3;
  readonly meanAnomaly: number;
  readonly meanMotion: number;
}

// position 是布局提示: 保留距星距离, 方向投影到小倾角轨道面; plane 必须为所属星系的正交单位基.
export function createPlanetOrbit(id: string, position: Position3, plane: OrbitalPlane): PlanetOrbit {
  const seed = orbitSeed(id);
  const phase = (seed / 0x100000000) * Math.PI * 2;
  const radius = Math.hypot(...position);
  if (radius < 1e-8) return { semiMajorAxis: 0, eccentricity: 0, periapsis: plane.reference, transverse: plane.transverse, meanAnomaly: 0, meanMotion: 0 };
  const node = phase * 2.37;
  const inclination = (((seed >>> 16) & 255) / 255) * maxPlanetInclination;
  const axis = plane.reference.map((value, index) => value * Math.cos(node) + (plane.transverse[index] ?? 0) * Math.sin(node)) as [number, number, number];
  const across = plane.reference.map(
    (value, index) =>
      (-value * Math.sin(node) + (plane.transverse[index] ?? 0) * Math.cos(node)) * Math.cos(inclination) + (plane.normal[index] ?? 0) * Math.sin(inclination),
  ) as [number, number, number];
  const x = position[0] * axis[0] + position[1] * axis[1] + position[2] * axis[2];
  const y = position[0] * across[0] + position[1] * across[1] + position[2] * across[2];
  // 布局提示接近轨道法线时, 用 ID 选定相位, 避免对接近零的投影归一化.
  const angle = Math.hypot(x, y) > radius * 1e-8 ? Math.atan2(y, x) : phase;
  const radial = axis.map((value, index) => value * Math.cos(angle) + (across[index] ?? 0) * Math.sin(angle)) as [number, number, number];
  const direction = axis.map((value, index) => -value * Math.sin(angle) + (across[index] ?? 0) * Math.cos(angle)) as [number, number, number];
  const eccentricity = 0.08 + (((seed >>> 8) & 255) / 255) * 0.24;
  const cos = Math.cos(phase);
  const sin = Math.sin(phase);
  const periapsis = radial.map((value, index) => value * cos - (direction[index] ?? 0) * sin) as [number, number, number];
  const transverse = radial.map((value, index) => value * sin + (direction[index] ?? 0) * cos) as [number, number, number];
  const semiMajorAxis = (radius * (1 + eccentricity * cos)) / (1 - eccentricity * eccentricity);
  const eccentricAnomaly = Math.atan2(Math.sqrt(1 - eccentricity * eccentricity) * sin, eccentricity + cos);
  return {
    semiMajorAxis,
    eccentricity,
    periapsis,
    transverse,
    meanAnomaly: eccentricAnomaly - eccentricity * Math.sin(eccentricAnomaly),
    meanMotion: 0.028 * (20 / semiMajorAxis) ** 1.5,
  };
}

// 固定五次 Newton 迭代解 Kepler 方程; e <= 0.32, 输出复用调用者缓冲, 不分配每帧对象.
export function writeOrbitPosition(orbit: PlanetOrbit, seconds: number, output: [number, number, number]): void {
  const mean = (orbit.meanAnomaly + ((seconds * orbit.meanMotion) % (Math.PI * 2))) % (Math.PI * 2);
  let anomaly = mean;
  for (let iteration = 0; iteration < 5; iteration++)
    anomaly -= (anomaly - orbit.eccentricity * Math.sin(anomaly) - mean) / (1 - orbit.eccentricity * Math.cos(anomaly));
  const x = orbit.semiMajorAxis * (Math.cos(anomaly) - orbit.eccentricity);
  const y = orbit.semiMajorAxis * Math.sqrt(1 - orbit.eccentricity * orbit.eccentricity) * Math.sin(anomaly);
  output[0] = orbit.periapsis[0] * x + orbit.transverse[0] * y;
  output[1] = orbit.periapsis[1] * x + orbit.transverse[1] * y;
  output[2] = orbit.periapsis[2] * x + orbit.transverse[2] * y;
}
