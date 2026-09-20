// 编辑框草稿与展示快照转换; 所有校验在重建场景之前完成, 不修改传入快照.
import type { GalaxyData, Star, StarStatus, Planet, Position3 } from "./types.ts";
import { createSystemPlane } from "./orbitalPlane.ts";
import { diskPosition } from "./layout.ts";
import { planetKinds } from "./presentation.ts";
import { validateGalaxyData } from "./validate.ts";

export interface DraftStar {
  id: string;
  name: string;
  status: StarStatus;
  satellites: number | null;
}

export interface GalaxyDraft {
  stars: DraftStar[];
  pulsar: { id: string; enabled: boolean; name: string };
}

// 原有普通恒星与黑洞容量合并; 状态切换不占用另一类节点额度.
export const editorLimits = { stars: 48, satellites: 180 } as const;
const colors = ["#ffbd78", "#83cfff", "#c3a2ff", "#a9ddff", "#ffe3a1", "#89b7ff", "#fff0cd", "#bdebf0", "#d9cbff"] as const;

// 每次打开生成独立草稿; 取消编辑不会改变当前星图.
export function draftFromGalaxy(data: GalaxyData): GalaxyDraft {
  const pulsar = data.stars.find((star) => star.status === "available" && star.appearance === "pulsar");
  return {
    stars: data.stars
      .filter((star) => star.status === "black-hole" || star.appearance !== "pulsar")
      .map((star) => ({ id: star.id, name: star.name, status: star.status, satellites: star.planets.length })),
    pulsar: { id: pulsar?.id ?? "star-pulsar", enabled: !!pulsar, name: pulsar?.name ?? "Pulsar" },
  };
}

// 新行取得当前草稿中未占用的 ID; 删除行或切换状态不会重编号其它恒星.
export function createDraftStar(draft: GalaxyDraft): DraftStar {
  const ids = new Set([...draft.stars.map((star) => star.id), draft.pulsar.id]);
  let sequence = 1;
  while (ids.has(`star-custom-${sequence}`)) sequence++;
  return { id: `star-custom-${sequence}`, name: `恒星 ${sequence}`, status: "available", satellites: 18 };
}

// 名称只用于展示; 不把用户文本拼入 HTML 或标识路径, 现有恒星 ID 始终保留.
function nodeName(value: string, label: string): string {
  const name = value.trim();
  if (!name || name.length > 64) throw new Error(`${label}名称应为 1–64 个字符`);
  return name;
}

// 卫星暂沿用现有业务行星模型, 在三类实体之间均匀分配; 不把编辑器参数写入后端.
export function galaxyFromDraft(draft: GalaxyDraft): GalaxyData {
  if (draft.stars.length > editorLimits.stars) throw new Error("节点数量超出编辑器上限");
  if (!draft.stars.length && !draft.pulsar.enabled) throw new Error("请至少添加一个星体");
  const stars: Star[] = draft.stars.map((entry, index) => {
    if (entry.status !== "available" && entry.status !== "black-hole") throw new Error(`第 ${index + 1} 颗恒星的状态无效`);
    const count = entry.status === "black-hole" ? 0 : entry.satellites;
    if (count === null || !Number.isInteger(count) || count < 0 || count > editorLimits.satellites)
      throw new Error(`第 ${index + 1} 颗恒星的卫星数量应为 0–${editorLimits.satellites} 的整数`);
    const id = entry.id;
    const plane = createSystemPlane(id);
    const planets: Planet[] = Array.from({ length: count }, (_, planetIndex) => {
      const kind = planetKinds[planetIndex % planetKinds.length];
      if (!kind) throw new Error("Missing planet kind");
      return {
        id: `${id}/satellite/${planetIndex + 1}`,
        starId: id,
        name: `卫星 ${planetIndex + 1}`,
        kind,
        position: diskPosition(planetIndex, count, 9, plane),
      };
    });
    const angle = (index * Math.PI * 2) / Math.max(1, draft.stars.length);
    const position: Position3 = [Math.cos(angle) * 100, 0, Math.sin(angle) * 100];
    return {
      id,
      name: nodeName(entry.name, `第 ${index + 1} 颗恒星`),
      color: colors[index % colors.length] ?? colors[0],
      status: entry.status,
      position,
      planets,
    };
  });
  const linkedStars = stars.filter((star) => star.status === "available");
  const links = linkedStars.flatMap((source, index) => linkedStars.slice(index + 1).map((target) => ({ source: source.id, target: target.id })));
  if (draft.pulsar.enabled)
    stars.push({
      id: draft.pulsar.id,
      name: nodeName(draft.pulsar.name, "脉冲星"),
      color: "#9bdcff",
      status: "available",
      appearance: "pulsar",
      position: [0, 0, 0],
      planets: [],
    });
  const data: GalaxyData = {
    stars,
    links,
    ...(draft.pulsar.enabled ? { centerStarId: draft.pulsar.id } : {}),
    sourceLabel: "自定义演示",
    description: "本地生成的星图, 未接入 Supervisor. 所有恒星（含黑洞状态）参与共同公转与等权质心布局; 黑洞状态不显示卫星或连线.",
  };
  validateGalaxyData(data);
  return data;
}
