// 屏幕空间三角形几何; 覆盖整个 NDC 方形且避免两个三角形之间的接缝.
import { BufferGeometry, Float32BufferAttribute } from "three";
import type { ResourceScope } from "../resourceScope.ts";

// 每个渲染用途独占几何, 生命周期立即登记到所属场景.
export function createFullscreenTriangle(scope: ResourceScope): BufferGeometry {
  const geometry = scope.own(new BufferGeometry());
  geometry.setAttribute("position", new Float32BufferAttribute([-1, -1, 0, 3, -1, 0, -1, 3, 0], 3));
  return geometry;
}
