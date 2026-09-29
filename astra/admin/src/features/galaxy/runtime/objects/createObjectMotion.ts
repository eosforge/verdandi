// 星体自转、黑洞时钟与独立公转周期; 只更新现有对象, 不绘制或拥有 RAF.
import * as THREE from "three";
import { writeOrbitPosition } from "../../model/orbit.ts";
import type { GalaxySelection } from "../../model/types.ts";
import type { createStarSystems } from "./createStarSystems.ts";
import { sceneConfig } from "../config.ts";
import { createStellarMotion } from "./createStellarMotion.ts";

// 所有行星共享速度倍率, 各自按独立周期包裹时间; 径向隔离不依赖相位同步.
export function createObjectMotion({ systems, blackHoles, pulsars, rotatingStars, stellarOrbit }: ReturnType<typeof createStarSystems>) {
  const orbitPosition: [number, number, number] = [0, 0, 0];
  const instanceMatrix = new THREE.Matrix4();
  const batches = systems.flatMap((system) => system.shells.map((shell) => ({ shell, times: new Float64Array(shell.orbits.length) })));
  const moveStars = createStellarMotion(systems, stellarOrbit);

  // 自转速度独立于数量和选择状态; 零速保留当前姿态, 负速反转, 无效输入不改变原速度.
  function setStarRotationSpeed(starId: string, radiansPerSecond: number): boolean {
    const star = rotatingStars.get(starId);
    if (!star || !Number.isFinite(radiansPerSecond)) return false;
    star.speed = radiansPerSecond;
    return true;
  }

  // 自转使用真实帧增量; 卫星公转为 15/6, 恒星固定独立倍率; 行星详情统一暂停公转.
  function update(deltaSeconds: number, selection: GalaxySelection): boolean {
    if (!Number.isFinite(deltaSeconds) || deltaSeconds <= 0) return false;
    for (const blackHole of blackHoles) blackHole.update(deltaSeconds);
    for (const pulsar of pulsars) pulsar.update(deltaSeconds);
    for (const star of rotatingStars.values()) {
      // 先按周期折叠时间, 避免有限但很大的输入在速度乘法时溢出.
      if (star.speed !== 0) star.model.rotateY((deltaSeconds % ((Math.PI * 2) / Math.abs(star.speed))) * star.speed);
    }
    if (selection?.planet) return false;
    const speed = selection ? sceneConfig.orbitSpeed.star : sceneConfig.orbitSpeed.overview;
    const moved = moveStars(deltaSeconds, sceneConfig.stellarRevolutionSpeed);
    for (const { shell, times } of batches) {
      shell.orbits.forEach((orbit, index) => {
        const period = (Math.PI * 2) / orbit.meanMotion;
        const seconds = ((times[index] ?? 0) + (deltaSeconds % (period / speed)) * speed) % period;
        times[index] = seconds;
        writeOrbitPosition(orbit, seconds, orbitPosition);
        shell.mesh.getMatrixAt(index, instanceMatrix);
        instanceMatrix.setPosition(...orbitPosition);
        shell.mesh.setMatrixAt(index, instanceMatrix);
      });
      shell.mesh.instanceMatrix.needsUpdate = true;
    }
    return moved;
  }

  return { update, setStarRotationSpeed };
}
