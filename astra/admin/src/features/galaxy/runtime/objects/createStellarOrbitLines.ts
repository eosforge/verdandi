// 恒星公转轨道的静态细线; 与实际运动共用椭圆参数, 不进入拾取目标或每帧更新.
import * as THREE from "three";
import type { StellarOrbit } from "../../model/stellarOrbits.ts";
import type { ResourceScope } from "../resourceScope.ts";

// 所有恒星（含黑洞）合并为一次绘制; 无公转规划时不创建资源.
export function createStellarOrbitLines(scene: THREE.Scene, orbit: StellarOrbit | undefined, scope: ResourceScope): THREE.LineSegments | undefined {
  if (!orbit?.members.length) return;
  const segments = 256;
  const positions = new Float32Array(orbit.members.length * segments * 2 * 3);
  let offset = 0;
  for (const { path } of orbit.members) {
    const minorAxis = path.semiMajorAxis * Math.sqrt(1 - path.eccentricity * path.eccentricity);
    // 按偏近点角均匀采样闭合几何, 与速度、当前相位无关; 公转中心位于椭圆焦点.
    for (let segment = 0; segment < segments; segment++) {
      for (let end = 0; end < 2; end++) {
        const anomaly = (((segment + end) % segments) / segments) * Math.PI * 2;
        const x = path.semiMajorAxis * (Math.cos(anomaly) - path.eccentricity);
        const y = minorAxis * Math.sin(anomaly);
        positions[offset++] = path.periapsis[0] * x + path.transverse[0] * y;
        positions[offset++] = path.periapsis[1] * x + path.transverse[1] * y;
        positions[offset++] = path.periapsis[2] * x + path.transverse[2] * y;
      }
    }
  }
  const geometry = scope.own(new THREE.BufferGeometry());
  geometry.setAttribute("position", new THREE.BufferAttribute(positions, 3));
  geometry.computeBoundingSphere();
  const material = scope.own(
    new THREE.LineBasicMaterial({ color: 0x6e96b0, transparent: true, opacity: 0.23, linewidth: 1, depthWrite: false, toneMapped: false }),
  );
  const lines = new THREE.LineSegments(geometry, material);
  lines.name = "StellarOrbitLines";
  lines.position.fromArray(orbit.center);
  scene.add(lines);
  return lines;
}
