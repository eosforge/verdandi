import { spherePosition } from "../model/layout.ts";
import { planetKinds } from "../model/presentation.ts";
import type { GalaxyData, PeerStar, Planet } from "../model/types.ts";

// 演示中的实体和位置完全确定, 不代表正在运行的 Peer 或实际订阅关系.
const demoPeers: readonly PeerStar[] = [
  makePeer("peer-atlas", "Atlas", "#ffbd78", [-48, 4, 0], 12, "available"),
  makePeer("peer-lyra", "Lyra", "#83cfff", [45, 13, -15], 16, "available"),
  makePeer("peer-vega", "Vega", "#c3a2ff", [5, -11, 55], 20, "available"),
  makePeer("peer-orion", "Orion", "#d29b86", [-64, -6, 62], 0, "unavailable"),
];

// 生成一个演示星系, 类型和位置只在构造快照时计算.
function makePeer(id: string, name: string, color: string, position: PeerStar["position"], perKind: number, status: PeerStar["status"]): PeerStar {
  const planets = planetKinds.flatMap((kind, ring) =>
    Array.from({ length: perKind }, (_, index): Planet => {
      return {
        id: `${id}/${kind.toLowerCase()}/${index + 1}`,
        name: `${kind.toLowerCase()}-${String(index + 1).padStart(2, "0")}`,
        kind,
        peerId: id,
        position: spherePosition(index, perKind, 13 + ring * 7, ring * 0.85),
      };
    }),
  );
  return { id, name, color, status, position, planets };
}

export const demoGalaxy: GalaxyData = {
  peers: demoPeers,
  links: [
    { source: "peer-atlas", target: "peer-lyra" },
    { source: "peer-lyra", target: "peer-vega" },
    { source: "peer-vega", target: "peer-atlas" },
  ],
  sourceLabel: "演示",
  description: "连接与属性均为本地演示数据, 未接入 Supervisor. Orion 为不可用节点, 只显示黑洞本体, 不包含行星或连线.",
};
