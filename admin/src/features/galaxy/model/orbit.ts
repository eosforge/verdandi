import type { Position3 } from "./types.ts";

export interface PlanetOrbit {
  readonly semiMajorAxis: number;
  readonly eccentricity: number;
  readonly periapsis: Position3;
  readonly transverse: Position3;
  readonly meanAnomaly: number;
  readonly meanMotion: number;
}

// 从已校验的初始位置和稳定 ID 构造轨道; 恒星位于焦点, 初始姿态不因启用公转而跳变.
export function createPlanetOrbit(id: string, position: Position3): PlanetOrbit {
  let seed = 2166136261;
  for (const character of id) seed = Math.imul(seed ^ character.charCodeAt(0), 16777619);
  const phase = ((seed >>> 0) / 0x100000000) * Math.PI * 2;
  const radius = Math.hypot(...position);
  if (radius < 1e-8) return { semiMajorAxis: 0, eccentricity: 0, periapsis: [1, 0, 0], transverse: [0, 1, 0], meanAnomaly: 0, meanMotion: 0 };
  const [x, y, z] = position.map((value) => value / radius) as [number, number, number];
  const reference: Position3 = Math.abs(y) < 0.9 ? [0, 1, 0] : [1, 0, 0];
  const tx = y * reference[2] - z * reference[1];
  const ty = z * reference[0] - x * reference[2];
  const tz = x * reference[1] - y * reference[0];
  const length = Math.hypot(tx, ty, tz);
  const tangent: Position3 = [tx / length, ty / length, tz / length];
  const normal: Position3 = [y * tangent[2] - z * tangent[1], z * tangent[0] - x * tangent[2], x * tangent[1] - y * tangent[0]];
  const tilt = phase * 2.37;
  const direction = tangent.map((value, index) => value * Math.cos(tilt) + (normal[index] ?? 0) * Math.sin(tilt)) as [number, number, number];
  const eccentricity = 0.08 + (((seed >>> 8) & 255) / 255) * 0.24;
  const cos = Math.cos(phase);
  const sin = Math.sin(phase);
  const radial: Position3 = [x, y, z];
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
