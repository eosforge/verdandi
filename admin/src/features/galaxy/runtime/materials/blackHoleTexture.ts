import * as THREE from "three";
import { blackHoleOptics } from "./blackHoleOptics.ts";

// 二维密度在角向严格周期, 在径向连续; 相同输入得到相同纹理, 不依赖全局随机源.
function noise(angle: number, radius: number, periods: number, seed: number): number {
  const x = angle * periods;
  const cellX = Math.floor(x);
  const cellY = Math.floor(radius);
  const fractionX = x - cellX;
  const fractionY = radius - cellY;
  const blendX = fractionX * fractionX * (3 - 2 * fractionX);
  const blendY = fractionY * fractionY * (3 - 2 * fractionY);
  // 仅包裹角向格点, 保持盘内外缘互相独立, 消除首尾接缝.
  const hash = (column: number, row: number) => {
    let mixed = Math.imul((((column % periods) + periods) % periods) ^ seed, 0x45d9f3b) ^ Math.imul(row, 0x27d4eb2d);
    mixed = Math.imul(mixed ^ (mixed >>> 16), 0x45d9f3b);
    return ((mixed ^ (mixed >>> 16)) >>> 0) / 0xffffffff;
  };
  const first = hash(cellX, cellY) * (1 - blendX) + hash(cellX + 1, cellY) * blendX;
  const second = hash(cellX, cellY + 1) * (1 - blendX) + hash(cellX + 1, cellY + 1) * blendX;
  return first * (1 - blendY) + second * blendY;
}

// 每场景烘焙一次连续的径向流纹, 角向首尾相接; mipmap 随真实投影压缩过滤细节, 纹理归调用者所有.
export function createPlasmaTexture(): THREE.DataTexture {
  const width = 256;
  const height = 1024;
  const data = new Uint8Array(width * height * 4);
  for (let row = 0; row < height; row++) {
    const radial = (row + 0.5) / height;
    const radius = blackHoleOptics.discInner + radial * (blackHoleOptics.discOuter - blackHoleOptics.discInner);
    for (let column = 0; column < width; column++) {
      const angle = (column + 0.5) / width;
      const shear = Math.log(radius / blackHoleOptics.discInner) * 0.55;
      const cloud = noise(angle - shear * 0.22, radial * 5, 6, 11);
      const warp = noise(angle - shear * 0.4, radial * 12, 9, 21);
      // 各层角向长度与径向厚度不同, 形成被剪切的局部流带; 最细层仍低于纹理采样频率.
      const strands = noise(angle - shear, radial * 60 + (warp - 0.5) * 12, 18, 37);
      const fine = noise(angle - shear * 0.85, radial * 160 + (warp - 0.5) * 9, 48, 71);
      const density = Math.min(1, (0.008 + cloud ** 2.4 * (0.25 + strands ** 2 * 0.7) + fine ** 4 * 0.22) * 1.7);
      // 中尺度流团在角向分段, 叠加更细的撕裂结构; 高亮在烘焙时筛选, 不把远景压成连续光滑的带子.
      const streamField = noise(angle - shear * 0.5, radial * 22 + (warp - 0.5) * 4, 24, 113);
      const breaks = noise(angle - shear * 0.8, radial * 40 + cloud * 5, 36, 157);
      const streamPeak = Math.max(0, (streamField - 0.3) / 0.7);
      const sparseStreams = Math.min(1, streamPeak * streamPeak * 3.5) * (0.2 + breaks * 0.8);
      const offset = (row * width + column) * 4;
      data[offset] = Math.round(density * 255);
      data[offset + 1] = Math.round(cloud * 255);
      data[offset + 2] = Math.round(sparseStreams * 255);
      data[offset + 3] = 255;
    }
  }
  const texture = new THREE.DataTexture(data, width, height, THREE.RGBAFormat, THREE.UnsignedByteType);
  texture.wrapS = THREE.RepeatWrapping;
  texture.generateMipmaps = true;
  texture.minFilter = THREE.LinearMipmapLinearFilter;
  texture.magFilter = THREE.LinearFilter;
  texture.anisotropy = 4;
  texture.needsUpdate = true;
  return texture;
}
