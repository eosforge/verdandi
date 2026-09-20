// 少量恒星优先共享紧凑内层; 容量增加后逐层扩展, 不模拟多体引力.
import { compactOrbitTemplates, compactSeparationBounds } from "./stellarPacking.ts";
import { writeOrbitPosition, type PlanetOrbit } from "./orbit.ts";
import { systemSpacing, type SystemEnvelope } from "./systemLayout.ts";
import type { Position3 } from "./types.ts";

export interface StellarOrbit {
  readonly centerId: string;
  readonly center: Position3;
  readonly members: readonly { readonly id: string; readonly path: PlanetOrbit }[];
  /** 覆盖恒星及所属卫星的整个公转扫掠范围, 用于裁剪与避碰而非强制镜头后退. */
  readonly extent: number;
}

export const stellarOrbitSeconds = 600;
// 每层最多十二颗; 超出后均衡分层, 避免仅剩一颗恒星被单独推向远处.
export const stellarLayerCapacity = 12;

// 紧凑层内共享周期以避免追尾, 形状/相位/倾角错开; 层间独立周期且完整径向带隔离.
export function planStellarOrbits(stars: readonly SystemEnvelope[], central: SystemEnvelope): StellarOrbit {
  const ordered = [...stars].sort((a, b) => a.extent - b.extent || (a.id < b.id ? -1 : a.id > b.id ? 1 : 0));
  const members: Array<{ id: string; path: PlanetOrbit }> = [];
  let occupiedRadius = central.extent * systemSpacing.extentRatio;
  let extent = central.extent;
  let firstAxis = 0;
  let start = 0;
  const layerCount = Math.ceil(ordered.length / stellarLayerCapacity);
  for (let layer = 0; layer < layerCount; layer++) {
    const count = Math.ceil((ordered.length - start) / (layerCount - layer));
    const candidates = ordered.slice(start, start + count);
    // 大小星系交错占据角向槽位, 避免最大的几个相邻并一起撑大整个内层.
    const group: SystemEnvelope[] = [];
    for (let slot = 0; slot < count; slot++) {
      const index = slot % 2 ? Math.floor(slot / 2) : count - 1 - Math.floor(slot / 2);
      const star = candidates[index];
      if (!star) throw new Error("Missing orbiting star");
      group.push(star);
    }
    let templates: PlanetOrbit[] = [];
    let axis = Infinity;
    // 保留小倾角基线, 尝试更大的上下错位; 只接受在整个周期中更紧凑且有安全余量的方案.
    for (const spread of [3, 9, 15]) {
      const candidate = compactOrbitTemplates(group, layer, spread);
      const separation = compactSeparationBounds(candidate);
      let required = 0;
      candidate.forEach((path, index) => {
        const star = group[index];
        if (!star) throw new Error("Missing orbiting star");
        required = Math.max(
          required,
          (occupiedRadius + star.extent * systemSpacing.extentRatio + systemSpacing.gap) / (path.semiMajorAxis * (1 - path.eccentricity)),
        );
        for (let other = index + 1; other < count; other++) {
          const neighbor = group[other];
          const clearance = separation[index]?.[other];
          if (!neighbor || clearance === undefined || clearance <= 0) {
            required = Infinity;
            continue;
          }
          required = Math.max(required, ((star.extent + neighbor.extent) * systemSpacing.extentRatio + systemSpacing.gap) / clearance);
        }
      });
      if (required < axis) {
        axis = required;
        templates = candidate;
      }
    }
    if (!Number.isFinite(axis)) throw new Error("Cannot safely pack stellar orbits");
    if (!firstAxis) firstAxis = axis;
    const meanMotion = ((Math.PI * 2) / stellarOrbitSeconds) * (firstAxis / axis) ** 1.5;
    templates.forEach((template, index) => {
      const star = group[index];
      if (!star) throw new Error("Missing orbiting star");
      const path = { ...template, semiMajorAxis: template.semiMajorAxis * axis, meanMotion };
      members.push({ id: star.id, path });
      occupiedRadius = Math.max(occupiedRadius, path.semiMajorAxis * (1 + path.eccentricity) + star.extent * systemSpacing.extentRatio);
      extent = Math.max(extent, path.semiMajorAxis * (1 + path.eccentricity) + star.extent);
    });
    start += count;
  }
  return { centerId: central.id, center: [...central.position], members, extent };
}

// Kepler 解复用既有数值实现, 输出写入共享中心, 不分配逐帧向量.
export function writeStellarPosition(orbit: StellarOrbit, index: number, seconds: number, target: [number, number, number]): void {
  const member = orbit.members[index];
  if (!member) throw new Error("Missing stellar orbit member");
  writeOrbitPosition(member.path, seconds, target);
  target[0] += orbit.center[0];
  target[1] += orbit.center[1];
  target[2] += orbit.center[2];
}
