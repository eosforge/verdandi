// 创建共享灯光、可用节点连线和行星选择环; 不持有业务选择状态.
import * as THREE from "three";
import type { GalaxyData, Position3 } from "../../model/types.ts";
import type { ResourceScope } from "../resourceScope.ts";
import { sceneConfig } from "../config.ts";

// 所有 GPU 资源归场景作用域; 即使输入保留旧边, 黑洞状态的端点也不生成连线.
export function createSceneDecorations(scene: THREE.Scene, data: GalaxyData, positions: ReadonlyMap<string, Position3>, scope: ResourceScope) {
  scene.add(new THREE.AmbientLight(0xbad2ff, 2));
  const keyLight = new THREE.DirectionalLight(0xffffff, 3);
  keyLight.position.set(-40, 100, 80);
  scene.add(keyLight);

  const starsById = new Map(data.stars.map((star) => [star.id, star]));
  const linkPositions: number[] = [];
  const endpoints: Position3[] = [];
  for (const link of data.links) {
    const source = starsById.get(link.source);
    const target = starsById.get(link.target);
    if (source?.status === "available" && target?.status === "available") {
      const start = positions.get(source.id);
      const end = positions.get(target.id);
      if (!start || !end) throw new Error("Missing link display position");
      linkPositions.push(...start, ...end);
      endpoints.push(start, end);
    }
  }
  const linkGeometry = scope.own(new THREE.BufferGeometry());
  const linkBuffer = new THREE.Float32BufferAttribute(linkPositions, 3);
  linkBuffer.setUsage(THREE.DynamicDrawUsage);
  linkGeometry.setAttribute("position", linkBuffer);
  const linkMaterial = scope.own(new THREE.LineBasicMaterial({ color: 0x75b9e6, transparent: true, opacity: 0.3, depthWrite: false }));
  const links = new THREE.LineSegments(linkGeometry, linkMaterial);
  links.visible = sceneConfig.showStarLinks;
  links.frustumCulled = false;
  scene.add(links);

  // 中心数组由恒星公转原位更新; 所有边只上传一次缓冲, 不逐帧重建几何.
  function updateLinks(): void {
    if (!links.visible || !endpoints.length) return;
    endpoints.forEach((position, index) => linkBuffer.setXYZ(index, ...position));
    linkBuffer.needsUpdate = true;
  }

  const selectionRing = new THREE.Mesh(
    scope.own(new THREE.RingGeometry(1.2, 1.3, 48)),
    scope.own(new THREE.MeshBasicMaterial({ color: 0xffffff, side: THREE.DoubleSide, transparent: true, opacity: 0.8, depthTest: false, depthWrite: false })),
  );
  selectionRing.visible = false;
  selectionRing.renderOrder = 2;
  scene.add(selectionRing);

  return { selectionRing, linkMaterial, updateLinks };
}
