// 所有恒星共用公转与质心布局, 黑洞状态不改变轨道身份; 规划与输入快照分离.
import type { GalaxyData } from "./types.ts";
import { spaceStarSystems, type SystemEnvelope } from "./systemLayout.ts";
import { planStellarOrbits, writeStellarPosition, type StellarOrbit } from "./stellarOrbits.ts";

// 正常与黑洞状态的恒星均围绕唯一脉冲星公转; 无脉冲星时统一采用静态布局.
export function planClusterLayout(data: GalaxyData, envelopes: readonly SystemEnvelope[]) {
  for (const envelope of envelopes)
    if (!Number.isFinite(envelope.extent) || envelope.extent < 0 || !envelope.position.every(Number.isFinite)) throw new RangeError("Invalid system envelope");
  const byId = new Map(envelopes.map((system) => [system.id, system]));
  const ordered = [...data.stars].sort((a, b) => (a.id < b.id ? -1 : a.id > b.id ? 1 : 0));
  const envelope = (id: string): SystemEnvelope => {
    const value = byId.get(id);
    if (!value) throw new Error("Missing system envelope: " + id);
    return value;
  };
  const pulsar = ordered.find((star) => star.status === "available" && star.appearance === "pulsar");
  const stars = ordered.filter((star) => star.id !== pulsar?.id).map((star) => envelope(star.id));
  const positions = new Map<string, [number, number, number]>();
  let orbit: StellarOrbit | undefined;
  if (pulsar) {
    const central = envelope(pulsar.id);
    positions.set(pulsar.id, [...central.position]);
    const plannedOrbit = planStellarOrbits(stars, central);
    orbit = plannedOrbit;
    plannedOrbit.members.forEach((star, index) => {
      const position: [number, number, number] = [0, 0, 0];
      writeStellarPosition(plannedOrbit, index, 0, position);
      positions.set(star.id, position);
    });
  } else {
    for (const [id, position] of spaceStarSystems(stars)) positions.set(id, [...position]);
  }
  return { positions, orbit };
}
