import type { PlanetKind } from "./types.ts";

export const planetKinds: readonly PlanetKind[] = ["Registry", "Subscriber", "Publisher"];
export const planetColors: Readonly<Record<PlanetKind, string>> = {
  Registry: "#ad9aff",
  Subscriber: "#6bded0",
  Publisher: "#f6bd74",
};

// 输入为所属行星总数; 以 30 颗为基准, 在指定数量节点间线性插值, 两端限制在 0.5..3 倍.
export function starScaleForPlanetCount(count: number): number {
  if (count <= 10) return 0.5;
  if (count <= 30) return 0.5 + (count - 10) / 40;
  if (count <= 60) return 1 + (count - 30) / 60;
  if (count <= 120) return 1.5 + (count - 60) / 120;
  return Math.min(3, 2 + (count - 120) / 60);
}
