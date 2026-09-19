// 星体自转、黑洞时钟与统一公转时钟; 只更新现有对象, 不绘制或拥有 RAF.
import * as THREE from "three";
import { writeOrbitPosition } from "../../model/orbit.ts";
import type { GalaxySelection } from "../../model/types.ts";
import type { createStarSystems } from "./createStarSystems.ts";
import { sceneConfig } from "../config.ts";

// 所有行星共享一个公转时间, 保持生成阶段的时序防碰撞约束.
export function createObjectMotion({ systems, blackHoles, rotatingStars, period }: ReturnType<typeof createStarSystems>) {
  const orbitPosition: [number, number, number] = [0, 0, 0];
  const instanceMatrix = new THREE.Matrix4();
  let orbitalSeconds = 0;

  // 自转速度独立于数量和选择状态; 零速保留当前姿态, 负速反转, 无效输入不改变原速度.
  function setStarRotationSpeed(starId: string, radiansPerSecond: number): boolean {
    const star = rotatingStars.get(starId);
    if (!star || !Number.isFinite(radiansPerSecond)) return false;
    star.speed = radiansPerSecond;
    return true;
  }

  // 自转使用真实帧增量; 公转总览五倍、恒星视图两倍、行星详情暂停, 每批仅上传一次矩阵.
  function update(deltaSeconds: number, selection: GalaxySelection): void {
    if (!Number.isFinite(deltaSeconds) || deltaSeconds <= 0) return;
    for (const blackHole of blackHoles) blackHole.update(deltaSeconds);
    for (const star of rotatingStars.values()) {
      // 先按周期折叠时间, 避免有限但很大的输入在速度乘法时溢出.
      if (star.speed !== 0) star.model.rotateY((deltaSeconds % ((Math.PI * 2) / Math.abs(star.speed))) * star.speed);
    }
    if (selection?.planet) return;
    orbitalSeconds = (orbitalSeconds + deltaSeconds * (selection ? sceneConfig.orbitSpeed.star : sceneConfig.orbitSpeed.overview)) % period;
    for (const system of systems) {
      for (const shell of system.shells) {
        shell.orbits.forEach((orbit, index) => {
          writeOrbitPosition(orbit, orbitalSeconds, orbitPosition);
          shell.mesh.getMatrixAt(index, instanceMatrix);
          instanceMatrix.setPosition(...orbitPosition);
          shell.mesh.setMatrixAt(index, instanceMatrix);
        });
        shell.mesh.instanceMatrix.needsUpdate = true;
      }
    }
  }

  return { update, setStarRotationSpeed };
}
