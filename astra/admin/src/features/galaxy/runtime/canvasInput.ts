import { Plane, Raycaster, Vector2, Vector3, type PerspectiveCamera } from "three";
import type { CameraMotion } from "./cameraMotion.ts";
import type { ResourceScope } from "./resourceScope.ts";

interface InputOptions {
  canvas: HTMLCanvasElement;
  camera: PerspectiveCamera;
  target: Vector3;
  motion: CameraMotion;
  pick(raycaster: Raycaster): void;
  overview(): void;
}

// 绑定场景输入; AbortController 统一撤销监听, 不接管 OrbitControls 的旋转和平移.
export function bindCanvasInput(options: InputOptions, scope: ResourceScope): { cancelPointer(): void } {
  const { canvas, camera, target, motion } = options;
  const events = new AbortController();
  scope.defer(() => events.abort());
  const signal = events.signal;
  const raycaster = new Raycaster();
  const pointer = new Vector2();
  const zoomPlane = new Plane();
  const zoomAnchor = new Vector3();
  const direction = new Vector3();
  let pointerDown: { id: number; x: number; y: number; button: number; moved: boolean } | null = null;
  let clickEligible = false;

  // 使用实时画布尺寸设置射线, 窗口或容器变化后仍保持准确.
  function setRay(event: MouseEvent): void {
    const rect = canvas.getBoundingClientRect();
    pointer.set(((event.clientX - rect.left) / Math.max(1, rect.width)) * 2 - 1, (-(event.clientY - rect.top) / Math.max(1, rect.height)) * 2 + 1);
    camera.updateMatrixWorld();
    raycaster.setFromCamera(pointer, camera);
  }

  // 捕获滚轮以阻止 OrbitControls 直接跳变距离; 按像素归一化后累计目标.
  function onWheel(event: WheelEvent): void {
    event.preventDefault();
    event.stopImmediatePropagation();
    setRay(event);
    zoomPlane.setFromNormalAndCoplanarPoint(camera.getWorldDirection(direction), target);
    if (!raycaster.ray.intersectPlane(zoomPlane, zoomAnchor)) zoomAnchor.copy(target);
    const pixels = event.deltaY * (event.deltaMode === 1 ? 16 : event.deltaMode === 2 ? canvas.clientHeight : 1);
    motion.zoomBy(pixels, zoomAnchor);
  }

  // 按下时立即中断自动推进和缩放, 先于 OrbitControls 接管; 多指输入不参与单击拾取.
  function onPointerDown(event: PointerEvent): void {
    preventMiddleDefault(event);
    motion.cancel();
    clickEligible = false;
    if (!event.isPrimary) {
      cancelPointer();
      return;
    }
    pointerDown = { id: event.pointerId, x: event.clientX, y: event.clientY, button: event.button, moved: false };
    canvas.focus({ preventScroll: true });
  }

  // 中键保留给画布平移, 阻止浏览器自动滚屏; 不停止传播, OrbitControls 仍接收完整手势.
  function preventMiddleDefault(event: MouseEvent): void {
    if (event.button === 1) event.preventDefault();
  }

  // 一旦越过拖动阈值便持续记为拖动, 即使最终回到起点也不会误选实体.
  function onPointerMove(event: PointerEvent): void {
    if (!pointerDown || pointerDown.id !== event.pointerId) return;
    if (Math.hypot(event.clientX - pointerDown.x, event.clientY - pointerDown.y) >= 5) {
      pointerDown.moved = true;
    }
  }

  // 仅未拖动的左键释放可触发随后的单击事件.
  function onPointerUp(event: PointerEvent): void {
    if (!pointerDown || pointerDown.id !== event.pointerId) return;
    onPointerMove(event);
    clickEligible = pointerDown.button === 0 && !pointerDown.moved;
    pointerDown = null;
  }

  // 双击的第二次 click 不拾取, 由 dblclick 统一返回全景.
  function onClick(event: MouseEvent): void {
    if (!clickEligible || event.detail > 1) return;
    clickEligible = false;
    setRay(event);
    options.pick(raycaster);
  }

  // 隐藏、失焦或取消手势后清空点击资格.
  function cancelPointer(): void {
    pointerDown = null;
    clickEligible = false;
  }

  // Escape 提供已聚焦画布的键盘返回入口.
  function onKeyDown(event: KeyboardEvent): void {
    if (event.key === "Escape") {
      event.preventDefault();
      options.overview();
    }
  }

  canvas.addEventListener("wheel", onWheel, { capture: true, passive: false, signal });
  canvas.addEventListener("pointerdown", onPointerDown, { capture: true, signal });
  // mousedown 兼容浏览器的鼠标默认行为, auxclick 覆盖中键释放时的默认动作.
  canvas.addEventListener("mousedown", preventMiddleDefault, { capture: true, signal });
  canvas.addEventListener("auxclick", preventMiddleDefault, { capture: true, signal });
  canvas.addEventListener("pointermove", onPointerMove, { signal });
  canvas.addEventListener("pointerup", onPointerUp, { signal });
  canvas.addEventListener("pointercancel", cancelPointer, { signal });
  canvas.addEventListener("blur", cancelPointer, { signal });
  canvas.addEventListener("click", onClick, { signal });
  canvas.addEventListener("dblclick", options.overview, { signal });
  canvas.addEventListener("keydown", onKeyDown, { signal });
  return { cancelPointer };
}
