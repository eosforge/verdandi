// 按星系包围范围扩展展示坐标, 保留输入快照和非退化布局的相对方位.
import type { Position3 } from "./types.ts";

export interface SystemEnvelope {
  readonly id: string;
  readonly position: Position3;
  /** 覆盖整条轨道与行星实体, 或黑洞光学体积的非负半径. */
  readonly extent: number;
}

export const systemSpacing = {
  // 两个星系边界之间额外留白为其半径之和的 2%, 不压缩实际模型与轨道包围范围.
  extentRatio: 1.02,
  // 最小附加留白, 单位为模型世界长度.
  gap: 1,
} as const;

// 围绕平均中心统一扩大, 不缩小原间距; 重合中心按稳定 ID 在圆周展开后使用同样的间距约束.
export function spaceStarSystems(systems: readonly SystemEnvelope[]): ReadonlyMap<string, Position3> {
  const ordered = [...systems].sort((a, b) => (a.id < b.id ? -1 : a.id > b.id ? 1 : 0));
  if (!ordered.length) return new Map();
  const center: [number, number, number] = [0, 0, 0];
  for (const system of ordered) {
    if (!Number.isFinite(system.extent) || system.extent < 0 || !system.position.every(Number.isFinite)) throw new RangeError("Invalid system envelope");
    for (const axis of [0, 1, 2] as const) center[axis] += system.position[axis] / ordered.length;
  }
  const duplicates = ordered.some((system, index) =>
    ordered.slice(index + 1).some((other) => Math.hypot(...system.position.map((value, axis) => value - (other.position[axis] ?? 0))) < 1e-8),
  );
  const positions: Position3[] = ordered.map((system, index) =>
    duplicates
      ? [center[0] + Math.cos((index * 2 * Math.PI) / ordered.length), center[1], center[2] + Math.sin((index * 2 * Math.PI) / ordered.length)]
      : system.position,
  );
  let scale = 1;
  ordered.forEach((system, index) => {
    const first = positions[index];
    if (!first) throw new Error("Missing system position");
    for (let other = index + 1; other < ordered.length; other++) {
      const second = positions[other];
      const neighbor = ordered[other];
      if (!second || !neighbor) throw new Error("Missing system envelope");
      const distance = Math.hypot(first[0] - second[0], first[1] - second[1], first[2] - second[2]);
      scale = Math.max(scale, ((system.extent + neighbor.extent) * systemSpacing.extentRatio + systemSpacing.gap) / distance);
    }
  });
  return new Map(
    ordered.map((system, index) => {
      const position = positions[index];
      if (!position) throw new Error("Missing system position");
      const spaced = position.map((value, axis) => (center[axis] ?? 0) + (value - (center[axis] ?? 0)) * scale) as [number, number, number];
      if (!spaced.every(Number.isFinite)) throw new RangeError("System layout exceeds finite coordinates");
      return [system.id, spaced];
    }),
  );
}
