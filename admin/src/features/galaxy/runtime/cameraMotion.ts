import { MathUtils, Vector3 } from "three";
import { sceneConfig } from "./config.ts";

interface Transition {
  elapsed: number;
  from: Vector3;
  to: Vector3;
  targetFrom: Vector3;
  targetTo: Vector3;
}

/** 就地更新相机位置和观察中心; 使用渲染时间增量, 后台暂停不跳跃. */
export class CameraMotion {
  private transition: Transition | null = null;
  private zoomDistance: number | null = null;
  private readonly zoomAnchor = new Vector3();
  private readonly position: Vector3;
  private readonly target: Vector3;

  // 仅借用向量, 不拥有 Three.js 或 DOM 资源.
  constructor(position: Vector3, target: Vector3) {
    this.position = position;
    this.target = target;
  }

  // 从当前姿态重新规划推进, 拷贝终点以防外部后续修改.
  moveTo(position: Vector3, target: Vector3): void {
    this.zoomDistance = null;
    this.transition = { elapsed: 0, from: this.position.clone(), to: position.clone(), targetFrom: this.target.clone(), targetTo: target.clone() };
  }

  // 累积归一化滚轮距离, 只改目标; 实际位移留给 update.
  zoomBy(pixels: number, anchor: Vector3): void {
    this.transition = null;
    this.zoomAnchor.copy(anchor);
    const scale = Math.exp(MathUtils.clamp(pixels, -150, 150) * 0.0015);
    this.zoomDistance = MathUtils.clamp(
      (this.zoomDistance ?? this.position.distanceTo(this.target)) * scale,
      sceneConfig.camera.minDistance,
      sceneConfig.camera.maxDistance,
    );
  }

  // 用户拖动取得控制权时终止尚未完成的推进和缩放.
  cancel(): void {
    this.transition = null;
    this.zoomDistance = null;
  }

  // 隐藏页面时丢弃滚轮惯性, 保留推进的已用时间供恢复后继续.
  pause(): void {
    this.zoomDistance = null;
  }

  // 五次平滑曲线推进与指数缩放均按秒计算, 同一时长不依赖帧数.
  update(deltaSeconds: number): void {
    if (this.transition) {
      const transition = this.transition;
      transition.elapsed += deltaSeconds;
      const progress = Math.min(1, transition.elapsed / sceneConfig.camera.transitionSeconds);
      const eased = progress ** 3 * (progress * (progress * 6 - 15) + 10);
      this.position.lerpVectors(transition.from, transition.to, eased);
      this.target.lerpVectors(transition.targetFrom, transition.targetTo, eased);
      if (progress === 1) this.transition = null;
    }
    if (this.zoomDistance !== null) {
      const distance = this.position.distanceTo(this.target);
      if (distance <= Number.EPSILON) {
        this.zoomDistance = null;
        return;
      }
      const remaining = this.zoomDistance - distance;
      const done = Math.abs(remaining) < 0.01;
      const nextDistance = done ? this.zoomDistance : distance + remaining * (1 - Math.exp(-deltaSeconds / sceneConfig.camera.zoomResponseSeconds));
      const ratio = nextDistance / distance;
      this.position.sub(this.zoomAnchor).multiplyScalar(ratio).add(this.zoomAnchor);
      this.target.sub(this.zoomAnchor).multiplyScalar(ratio).add(this.zoomAnchor);
      if (done) this.zoomDistance = null;
    }
  }
}
