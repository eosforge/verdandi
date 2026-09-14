import { spherePosition } from "../model/layout.ts";
import { planetKinds } from "../model/presentation.ts";
import type { GalaxyData, Star, Planet } from "../model/types.ts";

// 演示中的实体和位置完全确定, 不代表正在运行的 Star 或实际订阅关系.
const demoStars: readonly Star[] = [
  makeStar("star-atlas", "Atlas", "#ffbd78", [-48, 4, 0], 12, "available"),
  makeStar("star-lyra", "Lyra", "#83cfff", [45, 13, -15], 16, "available"),
  makeStar("star-vega", "Vega", "#c3a2ff", [5, -11, 55], 20, "available"),
  makeStar("star-orion", "Orion", "#d29b86", [-64, -6, 62], 0, "unavailable"),
];

// 生成一个演示星系, 类型和位置只在构造快照时计算.
function makeStar(id: string, name: string, color: string, position: Star["position"], perKind: number, status: Star["status"]): Star {
  const planets = planetKinds.flatMap((kind, ring) =>
    Array.from({ length: perKind }, (_, index): Planet => {
      return {
        id: `${id}/${kind.toLowerCase()}/${index + 1}`,
        name: `${kind.toLowerCase()}-${String(index + 1).padStart(2, "0")}`,
        kind,
        starId: id,
        position: spherePosition(index, perKind, 13 + ring * 7, ring * 0.85),
      };
    }),
  );
  return { id, name, color, status, position, planets };
}

export const demoGalaxy: GalaxyData = {
  stars: demoStars,
  links: [
    { source: "star-atlas", target: "star-lyra" },
    { source: "star-lyra", target: "star-vega" },
    { source: "star-vega", target: "star-atlas" },
  ],
  sourceLabel: "演示",
  description: "连接与属性均为本地演示数据, 未接入 Supervisor. Orion 为不可用节点, 只显示黑洞本体, 不包含行星或连线.",
};
