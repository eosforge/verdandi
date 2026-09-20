import { diskPosition } from "../model/layout.ts";
import { createSystemPlane } from "../model/orbitalPlane.ts";
import { planetKinds } from "../model/presentation.ts";
import type { GalaxyData, Star, Planet } from "../model/types.ts";

// 演示中的实体和位置完全确定, 不代表正在运行的 Star 或实际订阅关系.
const ordinaryStars: readonly Star[] = [
  makeStar("star-atlas", "Atlas", "#ffbd78", [-48, 4, 0], 12, "available"),
  makeStar("star-lyra", "Lyra", "#83cfff", [45, 13, -15], 16, "available"),
  makeStar("star-vega", "Vega", "#c3a2ff", [5, -11, 55], 20, "available"),
  makeStar("star-sirius", "Sirius", "#a9ddff", [-115, 8, -48], 4, "available"),
  makeStar("star-capella", "Capella", "#ffe3a1", [-20, 18, -100], 5, "available"),
  makeStar("star-rigel", "Rigel", "#89b7ff", [85, -8, -80], 6, "available"),
  makeStar("star-procyon", "Procyon", "#fff0cd", [116, 3, 38], 7, "available"),
  makeStar("star-altair", "Altair", "#bdebf0", [65, -14, 118], 8, "available"),
  makeStar("star-deneb", "Deneb", "#d9cbff", [-42, 12, 140], 6, "available"),
];

const orbitingStars = [...ordinaryStars, makeStar("star-orion", "Orion", "#d29b86", [-64, -6, 62], 0, "black-hole")];
// 尚无质量数据, 包含黑洞状态的全部恒星按等权平均确定脉冲星锚点.
const barycenter: [number, number, number] = [0, 0, 0];
for (const star of orbitingStars) for (const axis of [0, 1, 2] as const) barycenter[axis] += star.position[axis] / orbitingStars.length;
const demoStars: readonly Star[] = [...orbitingStars, { ...makeStar("star-pulsar", "Pulsar", "#9bdcff", barycenter, 0, "available"), appearance: "pulsar" }];

// 生成一个演示星系, 类型和位置只在构造快照时计算.
function makeStar(id: string, name: string, color: string, position: Star["position"], perKind: number, status: Star["status"]): Star {
  const plane = createSystemPlane(id);
  const planets = planetKinds.flatMap((kind, ring) =>
    Array.from({ length: perKind }, (_, index): Planet => {
      return {
        id: `${id}/${kind.toLowerCase()}/${index + 1}`,
        name: `${kind.toLowerCase()}-${String(index + 1).padStart(2, "0")}`,
        kind,
        starId: id,
        position: diskPosition(index, perKind, 9 + ring * 3, plane, ring * 0.85),
      };
    }),
  );
  return { id, name, color, status, position, planets };
}

// 九颗正常状态恒星两两连接, 每对只生成一条边; 黑洞状态恒星和脉冲星不参与连线.

export const demoGalaxy: GalaxyData = {
  stars: demoStars,
  centerStarId: "star-pulsar",
  links: ordinaryStars.flatMap((source, index) => ordinaryStars.slice(index + 1).map((target) => ({ source: source.id, target: target.id }))),
  sourceLabel: "演示",
  description:
    "连接与属性均为本地演示数据, 未接入 Supervisor. 正常恒星与黑洞状态的 Orion 共同围绕 Pulsar 公转并参与等权质心布局. Pulsar 与 Orion 无行星和连线.",
};
