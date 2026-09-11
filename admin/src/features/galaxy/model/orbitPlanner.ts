import { createPlanetOrbit, writeOrbitPosition, type PlanetOrbit } from "./orbit.ts";
import type { Position3 } from "./types.ts";

export interface OrbitRequest {
  readonly id: string;
  readonly position: Position3;
  readonly center: Position3;
}

export interface OrbitObstacle {
  readonly center: Position3;
  readonly radius: number;
}

interface SweptOrbit {
  orbit: PlanetOrbit;
  center: Position3;
  points: Float64Array;
  radius: number;
  extent: number;
}

const samples = 512;
const margin = 0.16;

// 两个同时运动的线段以共同时间插值, 返回相对线段到原点的最小距离平方, 而不是两条空间路径的距离.
function closestSquared(x: number, y: number, z: number, nextX: number, nextY: number, nextZ: number): number {
  const dx = nextX - x;
  const dy = nextY - y;
  const dz = nextZ - z;
  const length = dx * dx + dy * dy + dz * dz;
  const time = length > 1e-16 ? Math.max(0, Math.min(1, -(x * dx + y * dy + z * dz) / length)) : 0;
  return (x + dx * time) ** 2 + (y + dy * time) ** 2 + (z + dz * time) ** 2;
}

// 整个重复周期的分段扫掠体积; 二阶导数上界给出弦误差, 因而覆盖采样点之间的连续椭圆运动.
function sampleOrbit(orbit: PlanetOrbit, center: Position3, planetRadius: number, period: number): SweptOrbit {
  const points = new Float64Array((samples + 1) * 3);
  const local: [number, number, number] = [0, 0, 0];
  for (let index = 0; index <= samples; index++) {
    writeOrbitPosition(orbit, (index * period) / samples, local);
    points[index * 3] = local[0] + center[0];
    points[index * 3 + 1] = local[1] + center[1];
    points[index * 3 + 2] = local[2] + center[2];
  }
  const acceleration = (orbit.meanMotion ** 2 * orbit.semiMajorAxis) / (1 - orbit.eccentricity) ** 2;
  const error = (acceleration * (period / samples) ** 2) / 8;
  return { orbit, center, points, radius: planetRadius + margin + error, extent: orbit.semiMajorAxis * (1 + orbit.eccentricity) };
}

// 先用整个轨道包围球排除远处星系, 再检查对应时间段; 半径已包含模型体积和曲线插值误差.
function overlaps(first: SweptOrbit, second: SweptOrbit): boolean {
  const required = first.radius + second.radius;
  if (
    Math.hypot(first.center[0] - second.center[0], first.center[1] - second.center[1], first.center[2] - second.center[2]) >
    first.extent + second.extent + required
  )
    return false;
  for (let offset = 0; offset < samples * 3; offset += 3) {
    const a = first.points;
    const b = second.points;
    if (
      closestSquared(
        (a[offset] ?? 0) - (b[offset] ?? 0),
        (a[offset + 1] ?? 0) - (b[offset + 1] ?? 0),
        (a[offset + 2] ?? 0) - (b[offset + 2] ?? 0),
        (a[offset + 3] ?? 0) - (b[offset + 3] ?? 0),
        (a[offset + 4] ?? 0) - (b[offset + 4] ?? 0),
        (a[offset + 5] ?? 0) - (b[offset + 5] ?? 0),
      ) <
      required ** 2
    )
      return true;
  }
  return false;
}

// 恒星不参与时序避让, 近星点先作解析检查; 其它节点按静态球体与连续扫掠线段检查.
function hitsObstacle(candidate: SweptOrbit, obstacle: OrbitObstacle): boolean {
  const distance = Math.hypot(candidate.center[0] - obstacle.center[0], candidate.center[1] - obstacle.center[1], candidate.center[2] - obstacle.center[2]);
  if (distance < 1e-10) return candidate.orbit.semiMajorAxis * (1 - candidate.orbit.eccentricity) < candidate.radius + obstacle.radius;
  const required = candidate.radius + obstacle.radius;
  if (distance > candidate.extent + required) return false;
  const points = candidate.points;
  for (let offset = 0; offset < samples * 3; offset += 3) {
    if (
      closestSquared(
        (points[offset] ?? 0) - obstacle.center[0],
        (points[offset + 1] ?? 0) - obstacle.center[1],
        (points[offset + 2] ?? 0) - obstacle.center[2],
        (points[offset + 3] ?? 0) - obstacle.center[0],
        (points[offset + 4] ?? 0) - obstacle.center[1],
        (points[offset + 5] ?? 0) - obstacle.center[2],
      ) <
      required ** 2
    )
      return true;
  }
  return false;
}

// 初始化一次规划全体行星, 周期为共同周期的整数分频; 保留 n²a³ 常量和近快远慢, 允许空间轨道交叉.
export function planPlanetOrbits(requests: readonly OrbitRequest[], planetRadius: number, obstacles: readonly OrbitObstacle[]) {
  const ordered = [...requests].sort((a, b) => (a.id < b.id ? -1 : a.id > b.id ? 1 : 0));
  const initialOuter = Math.max(40, ...obstacles.map((obstacle) => (obstacle.radius + planetRadius + margin) * 2));
  for (let expansion = 0; expansion < 6; expansion++) {
    const outer = initialOuter * 1.2 ** expansion;
    const frequency = 0.028 * (20 / outer) ** 1.5;
    const period = (Math.PI * 2) / frequency;
    const accepted: SweptOrbit[] = [];
    const result = new Map<string, PlanetOrbit>();
    for (const request of ordered) {
      const radius = Math.max(4, Math.hypot(...request.position));
      const position: Position3 = Math.hypot(...request.position) > 1e-8 ? request.position : [radius, 0, 0];
      const original = createPlanetOrbit(request.id, position);
      const core = obstacles.find((obstacle) => obstacle.center.every((value, index) => value === request.center[index]));
      const minimumAxis = ((core?.radius ?? 0) + planetRadius + margin) / 0.76;
      const preferred = Math.max(1, Math.min(24, Math.round((outer / radius) ** 1.5), Math.floor((outer / minimumAxis) ** 1.5)));
      for (let attempt = 0; attempt < 160; attempt++) {
        const shape = attempt === 0 ? original : createPlanetOrbit(`${request.id}/candidate/${attempt}`, position);
        const harmonic = Math.max(1, preferred - Math.floor(attempt / 24) * Math.ceil(preferred / 6));
        const meanMotion = frequency * harmonic;
        const semiMajorAxis = 20 * (0.028 / meanMotion) ** (2 / 3);
        const orbit: PlanetOrbit = {
          ...shape,
          semiMajorAxis,
          eccentricity: Math.min(0.24, shape.eccentricity),
          meanMotion,
          meanAnomaly: shape.meanAnomaly + attempt * Math.PI * (3 - Math.sqrt(5)),
        };
        const candidate = sampleOrbit(orbit, request.center, planetRadius, period);
        if (obstacles.some((obstacle) => hitsObstacle(candidate, obstacle)) || accepted.some((other) => overlaps(candidate, other))) continue;
        accepted.push(candidate);
        result.set(request.id, orbit);
        break;
      }
      if (!result.has(request.id)) break;
    }
    if (result.size === requests.length) return { orbits: result, period };
  }
  throw new Error("Cannot place collision-free planet orbits within the planning budget");
}
