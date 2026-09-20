// 三层偶极磁场的细丝与散射外层, 合并为两个绘制批次; 临时建模几何在合并后立即释放.
import * as THREE from "three";
import { mergeGeometries } from "three/addons/utils/BufferGeometryUtils.js";
import type { ResourceScope } from "../resourceScope.ts";
import { pulsarConfig } from "./config.ts";
import { fieldVertex, fieldFragment } from "./shaders.ts";

// 借用实例流动相位; 几何随磁轴旋转, 不创建独立时钟或监听器.
export function createMagneticField(scope: ResourceScope, phase: { value: number }): THREE.Group {
  const group = new THREE.Group();
  group.name = "PulsarMagnetosphere";
  for (const glowLayer of [0, 1]) {
    const segments: THREE.BufferGeometry[] = [];
    let merged: THREE.BufferGeometry | null = null;
    try {
      for (let band = 0; band < 3; band++) {
        const count = 6 + band * 2;
        for (let index = 0; index < count; index++) {
          const seed = band * 13 + index;
          const equator = pulsarConfig.fieldRadius * (0.48 + band * 0.26) * (0.96 + 0.04 * Math.sin(seed * 2.4));
          const start = Math.asin(Math.sqrt(pulsarConfig.coreRadius / equator));
          const longitude = ((index + band * 0.3) * Math.PI * 2) / count;
          const points = Array.from({ length: 65 }, (_, step) => {
            const theta = start + ((Math.PI - 2 * start) * step) / 64;
            const radius = equator * Math.sin(theta) ** 2;
            const azimuth = longitude + (theta - Math.PI / 2) * 0.12 * (equator / pulsarConfig.fieldRadius);
            return new THREE.Vector3(radius * Math.sin(theta) * Math.cos(azimuth), radius * Math.cos(theta), radius * Math.sin(theta) * Math.sin(azimuth));
          });
          const thickness = glowLayer ? 0.22 + band * 0.045 : 0.055 + band * 0.012;
          const tube = new THREE.TubeGeometry(new THREE.CatmullRomCurve3(points), 96, thickness, 8, false);
          segments.push(tube);
          tube.setAttribute("fieldSeed", new THREE.Float32BufferAttribute(new Float32Array(tube.getAttribute("position").count).fill(seed), 1));
        }
      }
      merged = mergeGeometries(segments, false);
      if (!merged) throw new Error("Cannot merge pulsar magnetic field geometry");
      scope.own(merged);
    } finally {
      for (const segment of segments) segment.dispose();
    }
    const material = scope.own(
      new THREE.ShaderMaterial({
        uniforms: { phase, glowLayer: { value: glowLayer } },
        vertexShader: fieldVertex,
        fragmentShader: fieldFragment,
        transparent: true,
        depthWrite: false,
        blending: THREE.AdditiveBlending,
      }),
    );
    const field = new THREE.Mesh(merged, material);
    field.name = glowLayer ? "PulsarFieldGlow" : "PulsarField";
    group.add(field);
  }
  return group;
}
