// 恒星公转只平移星系父节点; 卫星轨道保持局部坐标, 核心、拾取和连线共用更新后的中心.
import { writeStellarPosition, type StellarOrbit } from "../../model/stellarOrbits.ts";
import type { StarSystem } from "./types.ts";

// 借用规划结果与场景对象, 不创建新几何、材质或渲染循环.
export function createStellarMotion(systems: readonly StarSystem[], orbit: StellarOrbit | undefined) {
  const byId = new Map(systems.map((system) => [system.star.id, system]));
  const members =
    orbit?.members.map(({ id, path }) => {
      const system = byId.get(id);
      if (!system) throw new Error("Missing orbiting star: " + id);
      return { system, period: (Math.PI * 2) / path.meanMotion, seconds: 0 };
    }) ?? [];
  return (deltaSeconds: number, speed: number): boolean => {
    if (!orbit || !members.length || !Number.isFinite(deltaSeconds) || deltaSeconds <= 0 || !Number.isFinite(speed) || speed <= 0) return false;
    members.forEach((member, index) => {
      member.seconds = (member.seconds + (deltaSeconds % (member.period / speed)) * speed) % member.period;
      writeStellarPosition(orbit, index, member.seconds, member.system.center);
      member.system.group.position.fromArray(member.system.center);
    });
    return true;
  };
}
