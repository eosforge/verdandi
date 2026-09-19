// WebGL 画布、相机与轨道控制器的创建和尺寸同步; 不持有选择状态或帧循环.
import * as THREE from "three";
import type { GalaxyData } from "../../model/types.ts";
import { FrameOrbitControls } from "../frameOrbitControls.ts";
import { sceneConfig } from "../config.ts";
import type { ResourceScope } from "../resourceScope.ts";

// 每取得一项资源立即登记清理; host 和快照仅在创建时借用.
export function createSceneView(host: HTMLElement, data: GalaxyData, scope: ResourceScope) {
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

  // 画布尺寸独立于详情面板; 侧栏折叠和窗口变化由观察器同步.
  function resize(): void {
    const width = Math.max(1, host.clientWidth);
    const height = Math.max(1, host.clientHeight);
    renderer.setPixelRatio(Math.min(window.devicePixelRatio, sceneConfig.maxPixelRatio));
    renderer.setSize(width, height);
    camera.aspect = width / height;
    camera.updateProjectionMatrix();
  }

  return { scene, camera, renderer, canvas, controls, resize };
}
