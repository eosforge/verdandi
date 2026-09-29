// 固定全局相机坐标, 根据星图范围维护观察中心、裁剪面与交互距离边界.
import { MathUtils, Vector3, type PerspectiveCamera, type Sphere } from "three";
import { sceneConfig } from "../config.ts";

export interface SceneFraming {
  readonly position: Vector3;
  readonly target: Vector3;
  minDistance: number;
  maxDistance: number;
  far: number;
}

// 初始与返回全局共用同一坐标; 空星图也预留缩放余量, 不会被控制器夹回.
export function createSceneFraming(): SceneFraming {
  const config = sceneConfig.camera;
  const position = new Vector3(...config.initialPosition);
  const target = new Vector3(...config.overviewTarget);
  const maxDistance = Math.max(config.maxDistance, position.distanceTo(target) * 1.1);
  return { position, target, minDistance: config.minDistance, maxDistance, far: Math.max(config.far, maxDistance) };
}

// 仅更新共享构图值, 不改相机姿态; 场景装配和返回命令决定何时应用, resize 不抢占用户操作.
export function fitSceneFraming(frame: SceneFraming, bounds: Sphere, camera: PerspectiveCamera): void {
  const config = sceneConfig.camera;
  frame.position.set(...config.initialPosition);
  if (bounds.radius > 0) frame.target.copy(bounds.center);
  else frame.target.set(...config.overviewTarget);
  const halfFov = Math.atan(Math.tan(MathUtils.degToRad(camera.fov) * 0.5) * Math.min(1, camera.aspect));
  const fullDistance = (bounds.radius / Math.sin(halfFov)) * 1.1;
  frame.maxDistance = Math.max(config.maxDistance, fullDistance * 2, frame.position.distanceTo(frame.target) * 1.1);
  frame.far = Math.max(config.far, frame.maxDistance + bounds.radius * 2);
}
