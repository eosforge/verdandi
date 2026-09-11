import type { Position3 } from "./types.ts";

// 按 Fibonacci 球面生成确定的位置; 参数必须属于有限且非空的球壳.
export function spherePosition(index: number, count: number, radius: number, phase = 0): Position3 {
  if (
    !Number.isInteger(count) ||
    count < 1 ||
    !Number.isInteger(index) ||
    index < 0 ||
    index >= count ||
    !Number.isFinite(radius) ||
    radius <= 0 ||
    !Number.isFinite(phase)
  ) {
    throw new RangeError("Invalid spherical layout parameters");
  }
  const angle = index * Math.PI * (3 - Math.sqrt(5)) + phase;
  const vertical = 1 - (2 * (index + 0.5)) / count;
  const horizontal = Math.sqrt(1 - vertical * vertical);
  return [Math.cos(angle) * horizontal * radius, vertical * radius, Math.sin(angle) * horizontal * radius];
}
