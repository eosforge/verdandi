import { planetKinds } from "./presentation.ts";
import type { GalaxyData, Position3 } from "./types.ts";

// 在申请 GPU 资源前检查展示快照的引用完整性; 此函数不代替未来网络 DTO 的解析.
export function validateGalaxyData(data: GalaxyData): void {
  const stars = new Set<string>();
  const planets = new Set<string>();
  for (const star of data.stars) {
    if (!star.id || stars.has(star.id)) throw new Error(`Duplicate or empty Star id: ${star.id}`);
    stars.add(star.id);
    validatePosition(star.position);
    if (!/^#[\da-f]{6}$/i.test(star.color)) throw new Error(`Invalid Star color: ${star.id}`);
    if (star.status !== "available" && star.status !== "unavailable") throw new Error(`Invalid Star status: ${star.id}`);
    for (const planet of star.planets) {
      if (!planet.id || planets.has(planet.id)) throw new Error(`Duplicate or empty planet id: ${planet.id}`);
      if (planet.starId !== star.id || !planetKinds.includes(planet.kind)) throw new Error(`Invalid planet owner or kind: ${planet.id}`);
      planets.add(planet.id);
      validatePosition(planet.position);
    }
  }
  const edges = new Map<string, Set<string>>();
  for (const link of data.links) {
    if (!stars.has(link.source) || !stars.has(link.target) || link.source === link.target) throw new Error("Invalid Star link endpoints");
    if (edges.get(link.source)?.has(link.target)) throw new Error("Duplicate undirected Star link");
    for (const [source, target] of [
      [link.source, link.target],
      [link.target, link.source],
    ] as const) {
      const neighbors = edges.get(source) ?? new Set<string>();
      neighbors.add(target);
      edges.set(source, neighbors);
    }
  }
}

// 坐标只接受三个有限数值, 避免 NaN 或无穷值污染相机和实例矩阵.
function validatePosition(position: Position3): void {
  if (position.length !== 3 || !position.every(Number.isFinite)) throw new Error("Invalid 3D position");
}
