import * as THREE from "three";
import type { GalaxyCallbacks, GalaxyController, GalaxyData, GalaxySelection } from "../model/types.ts";
import { validateGalaxyData } from "../model/validate.ts";
import { CameraMotion } from "./cameraMotion.ts";
import { bindCanvasInput } from "./canvasInput.ts";
import { sceneConfig } from "./config.ts";
import { createGalaxyObjects } from "./createGalaxyObjects.ts";
import { FrameLoop } from "./frameLoop.ts";
import { FrameOrbitControls } from "./frameOrbitControls.ts";
import { ResourceScope } from "./resourceScope.ts";
import { loadCelestialModels, type CelestialModels } from "./celestialAssets.ts";

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

// 组合对象、运动和输入, 选择状态由此处唯一维护并向 Vue 发布快照.
function initializeScene(host: HTMLElement, data: GalaxyData, callbacks: GalaxyCallbacks, scope: ResourceScope, models: CelestialModels): GalaxyController {
  const config = sceneConfig.camera;
  const scene = new THREE.Scene();
  scope.defer(() => scene.clear());
  const camera = new THREE.PerspectiveCamera(config.fieldOfView, 1, config.near, config.far);
  const overviewPosition = new THREE.Vector3(...config.overviewPosition);
  const overviewTarget = new THREE.Vector3(...config.overviewTarget);
  camera.position.copy(overviewPosition);
  const renderer = scope.own(new THREE.WebGLRenderer({ antialias: true, alpha: false, powerPreference: "high-performance" }));
  scope.defer(() => renderer.forceContextLoss());
  const canvas = renderer.domElement;
  scope.defer(() => canvas.remove());
  renderer.setPixelRatio(Math.min(window.devicePixelRatio, sceneConfig.maxPixelRatio));
  renderer.setClearColor(sceneConfig.background, 1);
  renderer.outputColorSpace = THREE.SRGBColorSpace;
  canvas.setAttribute("aria-label", "集群拓扑, 左键旋转, 中键平移, 滚轮缩放, 先点击恒星进入星系, 再点击所属行星查看详情, 双击或 Escape 返回全景");
  canvas.setAttribute("role", "img");
  const unavailableStars = data.stars.filter((star) => star.status === "unavailable");
  if (unavailableStars.length)
    canvas.setAttribute("aria-description", `不可用节点: ${unavailableStars.map((star) => star.name).join(", ")}. 以独立黑洞模型表示, 不显示行星或连线.`);
  canvas.tabIndex = 0;
  host.append(canvas);
  const controls = scope.own(new FrameOrbitControls(camera, canvas));
  controls.target.copy(overviewTarget);
  controls.enableDamping = true;
  controls.rotateSpeed = config.rotateSpeed;
  controls.panSpeed = config.panSpeed;
  controls.minDistance = config.minDistance;
  controls.maxDistance = config.maxDistance;
  controls.zoomToCursor = true;
  controls.mouseButtons.MIDDLE = THREE.MOUSE.PAN;
  controls.maxPolarAngle = Math.PI * 0.92;
  controls.updateFrame(0);
  const objects = createGalaxyObjects(scene, data, scope, models);
  const motion = new CameraMotion(camera.position, controls.target);
  let selection: GalaxySelection = null;
  let disposed = false;
  let failed = false;

  // 聚焦取得相机控制权前清空旧拖动惯性, 避免动画与残余旋转同时写入相机.
  function moveCamera(position: THREE.Vector3, target: THREE.Vector3): void {
    controls.clearMomentum();
    motion.moveTo(position, target);
  }

  // 发布完整选择快照, 让详情和场景装饰在同一操作中保持一致.
  function publishSelection(value: GalaxySelection): void {
    selection = value;
    objects.showSelection(value);
    callbacks.select(value);
  }

  // 验证 Star 标识后平滑接近, 仅选恒星时不产生行星详情.
  function selectStar(starId: string): boolean {
    if (disposed || failed) return false;
    const system = objects.systemsById.get(starId);
    if (!system) return false;
    const center = new THREE.Vector3(...system.star.position);
    const halfFov = Math.atan(Math.tan(THREE.MathUtils.degToRad(config.fieldOfView) * 0.5) * Math.min(1, camera.aspect));
    const distance = Math.max(config.starDistance, (system.orbitExtent / Math.sin(halfFov)) * 1.05);
    const approach = camera.position.clone().sub(controls.target).normalize().multiplyScalar(Math.min(config.maxDistance, distance));
    moveCamera(center.clone().add(approach), center);
    publishSelection({ star: system.star, planet: null });
    return true;
  }

  // 仅为存活场景中的可用恒星设置自转速度, 不改变选择、相机或其它节点的速度.
  function setStarRotationSpeed(starId: string, radiansPerSecond: number): boolean {
    return !disposed && !failed && objects.setStarRotationSpeed(starId, radiansPerSecond);
  }

  // 仅当前星系允许选择行星, 所有入口共用检查; 总览、跨星系或错误标识不改变状态.
  function selectPlanet(starId: string, planetId: string): boolean {
    if (disposed || failed || selection?.star.id !== starId) return false;
    const location = objects.findPlanet(starId, planetId);
    if (!location) return false;
    publishSelection({ star: location.star, planet: location.planet });
    return true;
  }

  // 返回固定全景并清空详情, 已在全景时不重启动画.
  function overview(): void {
    if (disposed || failed || !selection) return;
    moveCamera(overviewPosition, overviewTarget);
    publishSelection(null);
  }

  // 从当前公转位置的球壳外侧接近, 避免目标被恒星遮挡.
  function focusPlanet(starId: string, planetId: string): boolean {
    if (!selectPlanet(starId, planetId)) return false;
    const location = objects.findPlanet(starId, planetId);
    if (!location) return false;
    const target = objects.worldPosition(location);
    const approach = target
      .clone()
      .sub(new THREE.Vector3(...location.star.position))
      .normalize()
      .multiplyScalar(config.planetDistance)
      .add(new THREE.Vector3(0, 5, 0));
    moveCamera(target.clone().add(approach), target);
    return true;
  }

  // 唯一绘制入口, Vue 只接收每秒 FPS 和离散选择事件.
  function render(deltaSeconds: number): void {
    objects.update(deltaSeconds, selection);
    motion.update(deltaSeconds);
    controls.updateFrame(deltaSeconds);
    objects.faceCamera(camera.quaternion);
    renderer.render(scene, camera);
    if (import.meta.env.DEV) canvas.dataset.renderCount = String(renderer.info.render.frame);
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
        const hit = objects.pick(raycaster, selection);
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

  // 画布尺寸独立于详情面板; 侧栏折叠和窗口变化由观察器同步.
  function resize(): void {
    if (disposed || failed) return;
    const width = Math.max(1, host.clientWidth);
    const height = Math.max(1, host.clientHeight);
    renderer.setPixelRatio(Math.min(window.devicePixelRatio, sceneConfig.maxPixelRatio));
    renderer.setSize(width, height);
    camera.aspect = width / height;
    camera.updateProjectionMatrix();
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
