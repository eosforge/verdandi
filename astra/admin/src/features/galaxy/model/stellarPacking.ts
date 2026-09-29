// 紧凑共轨层的单位椭圆模板与连续时间间距下界; 只在重建星图时计算.
import { orbitSeed } from "./orbitalPlane.ts";
import { writeOrbitPosition, type PlanetOrbit } from "./orbit.ts";
import type { SystemEnvelope } from "./systemLayout.ts";
import type { Position3 } from "./types.ts";

const tau = Math.PI * 2;
const samples = 512;

// 以平衡的二阶扰动改变半径、相位和轨道面; 少量节点靠拢而不排成等半径多边形.
export function compactOrbitTemplates(stars: readonly SystemEnvelope[], layer: number, inclinationSpread: number): PlanetOrbit[] {
  const seed = orbitSeed(stars.map((star) => star.id).join("|"));
  const offset = (seed / 0x100000000) * tau;
  return stars.map((_, index) => {
    const angle = (index * tau) / stars.length;
    const variation = angle * 2 + offset;
    const node = layer * 2.399963 + offset + 0.025 * Math.cos(variation);
    const tilt = ((12 + inclinationSpread * Math.sin(variation)) * Math.PI) / 180;
    const heading = offset * 0.73;
    const u: Position3 = [Math.cos(node), 0, Math.sin(node)];
    const v: Position3 = [-Math.sin(node) * Math.cos(tilt), Math.sin(tilt), Math.cos(node) * Math.cos(tilt)];
    const periapsis = u.map((value, i) => value * Math.cos(heading) + (v[i] ?? 0) * Math.sin(heading)) as [number, number, number];
    const transverse = u.map((value, i) => -value * Math.sin(heading) + (v[i] ?? 0) * Math.cos(heading)) as [number, number, number];
    return {
      semiMajorAxis: 1 + 0.08 * Math.cos(variation),
      eccentricity: 0.03 + 0.01 * Math.sin(variation),
      periapsis,
      transverse,
      meanAnomaly: angle + 0.035 * Math.sin(variation) - heading,
      meanMotion: 1,
    };
  });
}
// 同层共享平均角速度, 因而相对构型每 2π 完整重复. 采样值扣除速度上界, 覆盖采样间隙.
export function compactSeparationBounds(paths: readonly PlanetOrbit[]): number[][] {
  const positions = paths.map(() => new Float64Array(samples * 3));
  const point: [number, number, number] = [0, 0, 0];
  paths.forEach((path, index) => {
    const buffer = positions[index];
    if (!buffer) throw new Error("Missing orbit sample buffer");
    for (let sample = 0; sample < samples; sample++) {
      writeOrbitPosition(path, (sample * tau) / samples, point);
      buffer.set(point, sample * 3);
    }
  });
  const speeds = paths.map((path) => path.semiMajorAxis * Math.sqrt((1 + path.eccentricity) / (1 - path.eccentricity)));
  const bounds = paths.map(() => paths.map(() => Infinity));
  // 配对距离对称, 每对轨道只扫描一次; 仍保持原采样与连续时间安全余量.
  for (let first = 0; first < paths.length; first++) {
    for (let second = first + 1; second < paths.length; second++) {
      const a = positions[first],
        b = positions[second];
      if (!a || !b) throw new Error("Missing orbit sample buffer");
      let closest = Infinity;
      for (let sample = 0; sample < samples; sample++) {
        const offset = sample * 3;
        const ax = a[offset],
          ay = a[offset + 1],
          az = a[offset + 2];
        const bx = b[offset],
          by = b[offset + 1],
          bz = b[offset + 2];
        if (ax === undefined || ay === undefined || az === undefined || bx === undefined || by === undefined || bz === undefined)
          throw new Error("Incomplete orbit sample");
        closest = Math.min(closest, Math.hypot(ax - bx, ay - by, az - bz));
      }
      // 最近采样的时间距离最多 π / samples; 单位模板的角速度为 1.
      const lowerBound = closest - (((speeds[first] ?? 0) + (speeds[second] ?? 0)) * Math.PI) / samples - 1e-9;
      const row = bounds[first],
        column = bounds[second];
      if (!row || !column) throw new Error("Missing orbit separation row");
      row[second] = lowerBound;
      column[first] = lowerBound;
    }
  }
  return bounds;
}
