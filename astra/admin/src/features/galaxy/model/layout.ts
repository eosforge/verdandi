// 演示布局沿星系共同轨道面生成; 实际椭圆和防碰撞相位由规划器决定.
import type { Position3 } from "./types.ts";
import type { OrbitalPlane } from "./orbitalPlane.ts";

// 在给定正交轨道面上均匀分配初始方位; 半径为正数, phase 为有限弧度.
export function diskPosition(index: number, count: number, radius: number, plane: OrbitalPlane, phase = 0): Position3 {
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
    throw new RangeError("Invalid disk layout parameters");
  }
  const angle = (index / count) * Math.PI * 2 + phase;
  return plane.reference.map((value, axis) => radius * (value * Math.cos(angle) + (plane.transverse[axis] ?? 0) * Math.sin(angle))) as [number, number, number];
}
