import * as THREE from "three";

// 灰度表面与实例颜色相乘, 所有行星共用贴图、几何和材质, 保留实例化绘制.
export function createPlanetTexture() {
  const width = 256;
  const height = 128;
  const pixels = new Uint8Array(width * height * 4);
  const craters = Array.from({ length: 18 }, (_, index) => {
    const vertical = 1 - (2 * (index + 0.5)) / 18;
    const angle = index * Math.PI * (3 - Math.sqrt(5));
    const horizontal = Math.sqrt(1 - vertical * vertical);
    return { x: Math.cos(angle) * horizontal, y: vertical, z: Math.sin(angle) * horizontal, radius: 0.09 + (index % 4) * 0.035 };
  });
  for (let y = 0; y < height; y++) {
    const latitude = ((y + 0.5) / height) * Math.PI;
    for (let x = 0; x < width; x++) {
      const longitude = ((x + 0.5) / width) * Math.PI * 2;
      const px = Math.sin(latitude) * Math.cos(longitude);
      const py = Math.cos(latitude);
      const pz = Math.sin(latitude) * Math.sin(longitude);
      // 在三维球面上采样, 避免经度接缝和极点的纹理不连续.
      let relief = 0.74 + Math.sin(px * 13 + Math.sin(pz * 9)) * Math.sin(py * 11 + pz * 7) * 0.08;
      relief += Math.sin(px * 53 + py * 31) * Math.sin(pz * 47 - py * 23) * 0.025;
      for (const crater of craters) {
        const distance = Math.hypot(px - crater.x, py - crater.y, pz - crater.z) / crater.radius;
        if (distance > 1.4) continue;
        const floor = 1 - THREE.MathUtils.smoothstep(distance, 0.35, 0.9);
        const rim = Math.exp(-(((distance - 0.98) / 0.15) ** 2));
        relief += rim * 0.14 - floor * 0.16;
      }
      const offset = (y * width + x) * 4;
      pixels[offset] = pixels[offset + 1] = pixels[offset + 2] = Math.round(THREE.MathUtils.clamp(relief, 0, 1) * 255);
      pixels[offset + 3] = 255;
    }
  }
  const texture = new THREE.DataTexture(pixels, width, height, THREE.RGBAFormat);
  texture.wrapS = THREE.RepeatWrapping;
  texture.magFilter = THREE.LinearFilter;
  texture.minFilter = THREE.LinearMipmapLinearFilter;
  texture.generateMipmaps = true;
  texture.needsUpdate = true;
  return texture;
}
