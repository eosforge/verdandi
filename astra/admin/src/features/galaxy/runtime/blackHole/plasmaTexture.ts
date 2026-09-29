// 初始化生成周期流纹密度贴图; 无外部图片与逐帧纹理更新.
import * as THREE from "three";
import { blackHoleOptics } from "./config.ts";

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

// RGBA 分别保存细丝、密度团、中尺度流带和稀薄丝雾, 角向周期; mipmap 过滤投影后的亚像素细节.
export function createPlasmaTexture(): THREE.DataTexture {
  const width = 256;
  const height = 1024;
  const data = new Uint8Array(width * height * 4);
  for (let row = 0; row < height; row++) {
    const radial = (row + 0.5) / height;
    const radius = blackHoleOptics.discInner + radial * (blackHoleOptics.discOuter - blackHoleOptics.discInner);
    // 内侧细丝间隔更紧, 向外连续展开, 不靠统一模糊模拟发散.
    const fiberRadius = (1 - Math.exp(-radial * 2.4)) / (1 - Math.exp(-2.4));
    const fan = radial * radial;
    for (let column = 0; column < width; column++) {
      const angle = (column + 0.5) / width;
      const shear = Math.log(radius / blackHoleOptics.discInner) * 0.18;
      const cloud = noise(angle - shear, radial * 11, 7, 11);
      const warp = noise(angle - shear * 0.4, radial * 8, 5, 21);
      // 径向扰动限制在一个细丝宽度附近, 不把长丝卷成大块大理石花纹.
      const strands = noise(angle - shear, fiberRadius * 115 + (warp - 0.5) * (1.2 + fan * 3), 17, 37);
      const fine = noise(angle - shear * 0.85, fiberRadius * 230 + (warp - 0.5) * (1 + fan * 3.5), 29, 71);
      const breaks = noise(angle - shear, radial * 31, 37, 157);
      // 窄峰和更短的角向连续段让纹理成为流丝, 避免高亮纹理连成整圈粗亮线.
      const density = Math.min(1, (strands ** 7 * 2.2 + fine ** 7 * 1.5) * (0.15 + cloud * 0.85) * (0.15 + breaks * 0.85));
      // 远景保留少量可分辨流带, 不以宽而均匀的发光底色替代细丝.
      const streamField = noise(angle - shear * 0.7, fiberRadius * 61 + (warp - 0.5) * (0.8 + fan * 2), 19, 113);
      const sparseStreams = Math.min(1, streamField ** 5 * 1.4) * (0.1 + breaks * 0.9);
      const wisps = noise(angle + shear * 0.3 + fan * 0.13, fiberRadius * 78 + (cloud - 0.5) * (1.5 + fan * 5), 6, 191);
      const offset = (row * width + column) * 4;
      data[offset] = Math.round(density * 255);
      data[offset + 1] = Math.round(cloud * 255);
      data[offset + 2] = Math.round(sparseStreams * 255);
      data[offset + 3] = Math.round(Math.min(1, wisps ** 3 * 1.6) * 255);
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
