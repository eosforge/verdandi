// 场景异步入口与生命周期装配; Vue 仅通过此文件动态加载运行时.
import { Vector3 } from "three";
import type { GalaxyCallbacks, GalaxyController, GalaxyData } from "../model/types.ts";
import { validateGalaxyData } from "../model/validate.ts";
import { CameraMotion } from "./cameraMotion.ts";
import { bindCanvasInput } from "./canvasInput.ts";
import { sceneConfig } from "./config.ts";
import { createGalaxyObjects } from "./createGalaxyObjects.ts";
import { FrameLoop } from "./frameLoop.ts";
import { createSceneView } from "./scene/createSceneView.ts";
import { createSelectionController } from "./scene/createSelectionController.ts";
import { ResourceScope } from "./resourceScope.ts";
import { loadCelestialModels, type CelestialModels } from "./celestialAssets.ts";
import { createLensingRenderer } from "./lensingRenderer.ts";

// 先校验并加载本地模型再创建画布; 取消、加载失败和初始化失败均释放已取得资源.
export async function createGalaxyScene(host: HTMLElement, data: GalaxyData, callbacks: GalaxyCallbacks, signal?: AbortSignal): Promise<GalaxyController> {
  validateGalaxyData(data);
  const scope = new ResourceScope();
  try {
    signal?.throwIfAborted();
    const models = await loadCelestialModels(data, scope, signal);
    signal?.throwIfAborted();
    return initializeScene(host, data, callbacks, scope, models);
  } catch (error) {
    reportCleanup(scope.dispose());
    throw error;
  }
}

// 组合画布、选择控制、对象、输入和唯一帧循环; 此处只拥有场景生命周期.
function initializeScene(host: HTMLElement, data: GalaxyData, callbacks: GalaxyCallbacks, scope: ResourceScope, models: CelestialModels): GalaxyController {
  const { scene, camera, renderer, canvas, controls, framing, frameBounds, refreshFraming, resize: resizeView } = createSceneView(host, data, scope);
  const objects = createGalaxyObjects(scene, data, scope, models);
  frameBounds(objects.bounds);
  const drawScene = createLensingRenderer(renderer, scene, camera, objects.horizons, objects.overlays, scope);
  const motion = new CameraMotion(camera.position, controls.target, framing);
  let disposed = false;
  let failed = false;
  let renderedFrames = 0;

  const interaction = createSelectionController({
    camera,
    controls,
    motion,
    framing,
    objects,
    isActive: () => !disposed && !failed,
    onSelect: (value) => {
      // 恒星视图锁定星体中心, 保留旋转与缩放; 总览和行星详情恢复平移.
      controls.enablePan = !value || !!value.planet;
      callbacks.select(value);
    },
  });
  const { selectStar, setStarRotationSpeed, selectPlanet, focusPlanet } = interaction;
  // 返回总览刷新边界, 使用统一的固定相机坐标和星图中心.
  function overview(): void {
    if (disposed || failed) return;
    refreshFraming();
    interaction.overview();
  }
  const followedCenter = new Vector3();
  const followDelta = new Vector3();
  const reportedCameraPosition = new Vector3(NaN, NaN, NaN);
  let cameraReportElapsed = 0.1;

  // 唯一绘制入口, Vue 只接收限频读数和离散选择事件.
  function render(deltaSeconds: number): void {
    const selection = interaction.selection;
    const followed = selection ? objects.systemsById.get(selection.star.id) : undefined;
    if (followed) followedCenter.fromArray(followed.center);
    objects.update(deltaSeconds, selection);
    if (followed) motion.translateFrame(followDelta.fromArray(followed.center).sub(followedCenter));
    motion.update(deltaSeconds);
    controls.updateFrame(deltaSeconds);
    if (followed && !selection?.planet) motion.keepCentered(followedCenter.fromArray(followed.center));
    objects.faceCamera(camera.quaternion);
    drawScene();
    // 读数与绘制使用同一相机, 限频且静止时不触发 Vue 更新.
    cameraReportElapsed += deltaSeconds;
    if (cameraReportElapsed >= 0.1) {
      cameraReportElapsed = 0;
      if (!reportedCameraPosition.equals(camera.position)) {
        reportedCameraPosition.copy(camera.position);
        callbacks.cameraPosition?.([camera.position.x, camera.position.y, camera.position.z]);
      }
    }
    if (import.meta.env.DEV) canvas.dataset.renderCount = String(++renderedFrames);
  }

  // 运行失败停止渲染和输入, 由界面提供重建入口.
  function fail(message: string): void {
    if (disposed || failed) return;
    failed = true;
    controls.enabled = false;
    loop.stop();
    callbacks.error(message);
  }

  const loop = scope.own(
    new FrameLoop(
      { request: (callback) => requestAnimationFrame(callback), cancel: (id) => cancelAnimationFrame(id) },
      render,
      callbacks.fps,
      (error) => {
        console.error("Galaxy rendering failed", error);
        fail("3D 场景绘制失败, 请重试。");
      },
      sceneConfig.targetFps,
    ),
  );
  const input = bindCanvasInput(
    {
      canvas,
      camera,
      target: controls.target,
      motion,
      pick(raycaster) {
        const hit = objects.pick(raycaster, interaction.selection);
        if (hit?.planet) selectPlanet(hit.star.id, hit.planet.id);
        else if (hit) selectStar(hit.star.id);
      },
      overview,
    },
    scope,
  );
  const events = new AbortController();
  scope.defer(() => events.abort());

  // 隐藏时取消请求和滚轮惯性, 恢复时从暂停姿态继续.
  function onVisibility(): void {
    if (document.hidden) {
      loop.stop();
      motion.pause();
      controls.clearMomentum();
      input.cancelPointer();
    } else if (!failed && !disposed) loop.start();
  }

  // 上下文丢失后交给用户重建, 不在无效上下文上继续调度.
  function onContextLost(event: Event): void {
    event.preventDefault();
    fail("3D 图形上下文已中断, 请重试。");
  }

  // 观察器晚到回调不得操作已停止服务的 WebGL 上下文.
  function resize(): void {
    if (!disposed && !failed) resizeView();
  }

  const observer = new ResizeObserver(resize);
  scope.defer(() => observer.disconnect());
  observer.observe(host);
  document.addEventListener("visibilitychange", onVisibility, { signal: events.signal });
  canvas.addEventListener("webglcontextlost", onContextLost, { signal: events.signal });
  resize();
  onVisibility();

  // 幂等关闭, 先拒绝后续控制调用, 再逆序释放全部资源.
  function dispose(): void {
    if (disposed) return;
    disposed = true;
    reportCleanup(scope.dispose());
  }

  return { selectStar, setStarRotationSpeed, selectPlanet, focusPlanet, overview, dispose };
}

// 清理异常仅报告给开发者, 不遮盖原始初始化失败或阻断其它资源释放.
function reportCleanup(failures: unknown[]): void {
  if (failures.length) console.error("Galaxy cleanup failed", failures);
}
