import { Quaternion, Vector3, type PerspectiveCamera } from "three";
import { OrbitControls } from "three/addons/controls/OrbitControls.js";
import { sceneConfig } from "./config.ts";

/** 输入只累积旋转和平移量; 唯一渲染帧以时间阻尼消费, 避免事件频率改变运动速度. */
export class FrameOrbitControls extends OrbitControls<PerspectiveCamera> {
  private readonly savedPosition = new Vector3();
  private readonly savedTarget = new Vector3();
  private readonly savedQuaternion = new Quaternion();

  // OrbitControls 的事件处理器会调用此入口; 延后到 updateFrame, 不在输入事件中移动相机.
  override update(): boolean {
    return false;
  }

  // 每个已绘制帧仅调用一次; 阻尼时间常数按秒定义, 不依赖鼠标采样率或显示刷新率.
  updateFrame(deltaSeconds: number): boolean {
    this.dampingFactor = -Math.expm1(-Math.max(0, deltaSeconds) / sceneConfig.camera.dragResponseSeconds);
    return super.update(deltaSeconds);
  }

  // 聚焦或隐藏前通过公开 update 消耗旧惯性, 随即恢复当前姿态; 不访问 Three.js 私有字段.
  clearMomentum(): void {
    this.savedPosition.copy(this.object.position);
    this.savedTarget.copy(this.target);
    this.savedQuaternion.copy(this.object.quaternion);
    const damping = this.enableDamping;
    const autoRotate = this.autoRotate;
    try {
      this.enableDamping = false;
      this.autoRotate = false;
      super.update(0);
    } finally {
      this.enableDamping = damping;
      this.autoRotate = autoRotate;
      this.object.position.copy(this.savedPosition);
      this.target.copy(this.savedTarget);
      this.object.quaternion.copy(this.savedQuaternion);
      this.object.updateMatrixWorld();
    }
  }
}
