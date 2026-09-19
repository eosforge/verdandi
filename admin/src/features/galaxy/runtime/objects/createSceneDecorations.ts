// 创建共享灯光、可用节点连线和行星选择环; 不持有业务选择状态.
import * as THREE from "three";
import type { GalaxyData } from "../../model/types.ts";
import type { ResourceScope } from "../resourceScope.ts";

// 所有 GPU 资源归场景作用域, 不可用端点不生成连线.
export function createSceneDecorations(scene: THREE.Scene, data: GalaxyData, scope: ResourceScope) {
  scene.add(new THREE.AmbientLight(0xbad2ff, 2));
  const keyLight = new THREE.DirectionalLight(0xffffff, 3);
  keyLight.position.set(-40, 100, 80);
  scene.add(keyLight);

  const starsById = new Map(data.stars.map((star) => [star.id, star]));
  const linkPositions: number[] = [];
  for (const link of data.links) {
    const source = starsById.get(link.source);
    const target = starsById.get(link.target);
    if (source?.status === "available" && target?.status === "available") {
      linkPositions.push(...source.position, ...target.position);
    }
  }
  const linkGeometry = scope.own(new THREE.BufferGeometry());
  linkGeometry.setAttribute("position", new THREE.Float32BufferAttribute(linkPositions, 3));
  const linkMaterial = scope.own(new THREE.LineBasicMaterial({ color: 0x75b9e6, transparent: true, opacity: 0.3, depthWrite: false }));
  scene.add(new THREE.LineSegments(linkGeometry, linkMaterial));

  const selectionRing = new THREE.Mesh(
    scope.own(new THREE.RingGeometry(1.2, 1.3, 48)),
    scope.own(new THREE.MeshBasicMaterial({ color: 0xffffff, side: THREE.DoubleSide, transparent: true, opacity: 0.8, depthTest: false, depthWrite: false })),
  );
  selectionRing.visible = false;
  selectionRing.renderOrder = 2;
  scene.add(selectionRing);

  return { selectionRing, linkMaterial };
}
