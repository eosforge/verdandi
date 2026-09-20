// WebGL 画布、相机与轨道控制器的创建和尺寸同步; 不持有选择状态或帧循环.
import * as THREE from "three";
import type { GalaxyData } from "../../model/types.ts";
import { FrameOrbitControls } from "../frameOrbitControls.ts";
import { sceneConfig } from "../config.ts";
import type { ResourceScope } from "../resourceScope.ts";
import { createSceneFraming, fitSceneFraming } from "./sceneFraming.ts";
import { createCosmicBackground } from "../background/createCosmicBackground.ts";

// 每取得一项资源立即登记清理; host 和快照仅在创建时借用.
export function createSceneView(host: HTMLElement, data: GalaxyData, scope: ResourceScope) {
  const config = sceneConfig.camera;
  const scene = new THREE.Scene();
  scope.defer(() => scene.clear());
  const camera = new THREE.PerspectiveCamera(config.fieldOfView, 1, config.near, config.far);
  const framing = createSceneFraming();
  let bounds = new THREE.Sphere(new THREE.Vector3(), 0);
  camera.position.copy(framing.position);
  const renderer = scope.own(new THREE.WebGLRenderer({ antialias: true, alpha: false, powerPreference: "high-performance" }));
  scope.defer(() => renderer.forceContextLoss());
  const canvas = renderer.domElement;
  scope.defer(() => canvas.remove());
  renderer.setPixelRatio(Math.min(window.devicePixelRatio, sceneConfig.maxPixelRatio));
  renderer.setClearColor(sceneConfig.background, 1);
  renderer.outputColorSpace = THREE.SRGBColorSpace;
  canvas.setAttribute("aria-label", "集群拓扑, 左键旋转, 中键平移, 滚轮缩放, 先点击恒星进入星系, 再点击所属行星查看详情, 双击或 Escape 返回全景");
  canvas.setAttribute("role", "img");
  const blackHoleStars = data.stars.filter((star) => star.status === "black-hole");
  if (blackHoleStars.length)
    canvas.setAttribute("aria-description", `黑洞状态的恒星: ${blackHoleStars.map((star) => star.name).join(", ")}. 不显示行星或连线.`);
  canvas.tabIndex = 0;
  host.append(canvas);
  const controls = scope.own(new FrameOrbitControls(camera, canvas));
  controls.target.copy(framing.target);
  controls.enableDamping = true;
  controls.rotateSpeed = config.rotateSpeed;
  controls.panSpeed = config.panSpeed;
  controls.minDistance = config.minDistance;
  controls.maxDistance = framing.maxDistance;
  controls.zoomToCursor = true;
  controls.mouseButtons.MIDDLE = THREE.MOUSE.PAN;
  controls.maxPolarAngle = Math.PI * 0.92;
  controls.updateFrame(0);
  createCosmicBackground(scene, camera, scope);

  // 画布尺寸独立于详情面板; 容器和窗口变化由观察器同步.
  function resize(): void {
    const width = Math.max(1, host.clientWidth);
    const height = Math.max(1, host.clientHeight);
    renderer.setPixelRatio(Math.min(window.devicePixelRatio, sceneConfig.maxPixelRatio));
    renderer.setSize(width, height);
    camera.aspect = width / height;
    refreshFraming();
  }

  // 更新观察中心和距离边界, 全局位置始终使用用户指定坐标, 不由自动构图重新决定.
  function refreshFraming(): void {
    fitSceneFraming(framing, bounds, camera);
    controls.maxDistance = framing.maxDistance;
    camera.far = framing.far;
    camera.updateProjectionMatrix();
  }

  // 初始化和返回总览共用同一构图位置; resize 只刷新目标, 不抢占当前手动相机姿态.
  function frameBounds(value: THREE.Sphere): void {
    bounds = value.clone();
    resize();
    camera.position.copy(framing.position);
    controls.target.copy(framing.target);
    controls.updateFrame(0);
  }

  return { scene, camera, renderer, canvas, controls, resize, framing, frameBounds, refreshFraming };
}
