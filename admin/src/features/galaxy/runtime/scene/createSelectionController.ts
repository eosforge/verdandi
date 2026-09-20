// 选择状态的唯一所有者; 所有入口共用 ID、星系范围和生命周期检查.
import * as THREE from "three";
import type { GalaxySelection, GalaxyCallbacks } from "../../model/types.ts";
import type { GalaxyObjects } from "../createGalaxyObjects.ts";
import type { CameraMotion } from "../cameraMotion.ts";
import { sceneConfig } from "../config.ts";
import type { SceneFraming } from "./sceneFraming.ts";

interface SelectionOptions {
  readonly camera: THREE.PerspectiveCamera;
  readonly controls: { readonly target: THREE.Vector3; clearMomentum(): void };
  readonly motion: Pick<CameraMotion, "moveTo">;
  /** 与画布共同维护的总览目标和缩放边界, 随星图范围与窗口比例更新. */
  readonly framing: SceneFraming;
  readonly objects: Pick<GalaxyObjects, "systemsById" | "findPlanet" | "worldPosition" | "showSelection" | "setStarRotationSpeed">;
  /** 场景失败或开始释放即返回 false, 禁止后续命令修改对象. */
  readonly isActive: () => boolean;
  readonly onSelect: GalaxyCallbacks["select"];
}

// 接近目标只启动已有的平滑相机运动, 不直接修改相机位置.
export function createSelectionController({ camera, controls, motion, framing, objects, isActive, onSelect }: SelectionOptions) {
  const config = sceneConfig.camera;
  let selection: GalaxySelection = null;
  // 聚焦取得相机控制权前清空旧拖动惯性, 避免动画与残余旋转同时写入相机.
  function moveCamera(position: THREE.Vector3, target: THREE.Vector3): void {
    controls.clearMomentum();
    motion.moveTo(position, target);
  }

  // 发布完整选择快照, 让详情和场景装饰在同一操作中保持一致.
  function publishSelection(value: GalaxySelection): void {
    selection = value;
    objects.showSelection(value);
    onSelect(value);
  }

  // 验证 Star 标识后平滑接近, 仅选恒星时不产生行星详情.
  function selectStar(starId: string): boolean {
    if (!isActive()) return false;
    const system = objects.systemsById.get(starId);
    if (!system) return false;
    const center = new THREE.Vector3(...system.center);
    const halfFov = Math.atan(Math.tan(THREE.MathUtils.degToRad(config.fieldOfView) * 0.5) * Math.min(1, camera.aspect));
    const distance = Math.max(config.starDistance, (system.orbitExtent / Math.sin(halfFov)) * 1.05);
    const approach = camera.position.clone().sub(controls.target).normalize().multiplyScalar(Math.min(framing.maxDistance, distance));
    moveCamera(center.clone().add(approach), center);
    publishSelection({ star: system.star, planet: null });
    return true;
  }

  // 仅为存活场景中的可用恒星设置自转速度, 不改变选择、相机或其它节点的速度.
  function setStarRotationSpeed(starId: string, radiansPerSecond: number): boolean {
    return isActive() && objects.setStarRotationSpeed(starId, radiansPerSecond);
  }

  // 仅当前星系允许选择行星, 所有入口共用检查; 总览、跨星系或错误标识不改变状态.
  function selectPlanet(starId: string, planetId: string): boolean {
    if (!isActive() || selection?.star.id !== starId) return false;
    const location = objects.findPlanet(starId, planetId);
    if (!location) return false;
    publishSelection({ star: location.star, planet: location.planet });
    return true;
  }

  // 即使未选节点也强制恢复全景构图, 支持平移/缩放后复位及被手势中断后的重试.
  function overview(): void {
    if (!isActive()) return;
    moveCamera(framing.position, framing.target);
    if (selection) publishSelection(null);
  }

  // 从当前公转位置的球壳外侧接近, 避免目标被恒星遮挡.
  function focusPlanet(starId: string, planetId: string): boolean {
    if (!selectPlanet(starId, planetId)) return false;
    const location = objects.findPlanet(starId, planetId);
    if (!location) return false;
    const system = objects.systemsById.get(starId);
    if (!system) return false;
    const target = objects.worldPosition(location);
    const approach = target
      .clone()
      .sub(new THREE.Vector3(...system.center))
      .normalize()
      .multiplyScalar(config.planetDistance)
      .add(new THREE.Vector3(0, 5, 0));
    moveCamera(target.clone().add(approach), target);
    return true;
  }

  return {
    get selection(): GalaxySelection {
      return selection;
    },
    selectStar,
    setStarRotationSpeed,
    selectPlanet,
    focusPlanet,
    overview,
  };
}
