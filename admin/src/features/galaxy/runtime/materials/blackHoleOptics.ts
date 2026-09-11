import * as THREE from "three";

export const blackHoleOptics = {
  shadowRadius: 4.5,
  schwarzschildRadius: Math.sqrt(3),
  minImpact: 0.02,
  maxImpact: 28,
  volumeRadius: 30,
  discInner: Math.sqrt(3) * 3,
  discOuter: 24,
  discFadeStart: 19.5,
  discHalfHeight: 0.28,
  maxAngle: Math.PI * 3,
  width: 513,
  height: 768,
  observerHeight: 128,
  observerMinRadius: 6,
} as const;

// 从无穷远积分平面光轨道 u'' + u = 1.5 rs u²; 参数均为正数, rs 可为零用于平直空间对照.
export function traceLightOrbit(impact: number, schwarzschildRadius: number, samples = blackHoleOptics.height) {
  if (!Number.isFinite(impact) || impact <= 0 || !Number.isFinite(schwarzschildRadius) || schwarzschildRadius < 0 || !Number.isInteger(samples) || samples < 2)
    throw new Error("Invalid light-orbit parameters");
  const inverseRadii = new Float32Array(samples);
  const step = blackHoleOptics.maxAngle / (samples - 1);
  let radiusInverse = 0;
  let velocity = 1 / impact;
  let captured = false;
  let escapeAngle: number | null = null;
  // 标量加速度无副作用, 与角向步长的四阶积分共用.
  const acceleration = (value: number) => -value + 1.5 * schwarzschildRadius * value * value;
  for (let index = 1; index < samples; index++) {
    const a1 = acceleration(radiusInverse);
    const v2 = velocity + a1 * step * 0.5;
    const a2 = acceleration(radiusInverse + velocity * step * 0.5);
    const v3 = velocity + a2 * step * 0.5;
    const a3 = acceleration(radiusInverse + v2 * step * 0.5);
    const v4 = velocity + a3 * step;
    const a4 = acceleration(radiusInverse + v3 * step);
    radiusInverse += ((velocity + 2 * v2 + 2 * v3 + v4) * step) / 6;
    velocity += ((a1 + 2 * a2 + 2 * a3 + a4) * step) / 6;
    if (radiusInverse <= 0) {
      escapeAngle = index * step;
      break;
    }
    if (schwarzschildRadius > 0 && radiusInverse >= 1 / schwarzschildRadius) {
      captured = true;
      inverseRadii.fill(1 / schwarzschildRadius, index);
      break;
    }
    inverseRadii[index] = radiusInverse;
  }
  return { inverseRadii, captured, escapeAngle };
}

// 临界值两侧分别加密, 同时覆盖被捕获的前景光线和绕过黑洞后逃逸的光线.
export function impactAtColumn(column: number): number {
  const { minImpact, shadowRadius, maxImpact, width } = blackHoleOptics;
  const position = (column / (width - 1)) * 2 - 1;
  return position < 0 ? shadowRadius - (shadowRadius - minImpact) * position * position : shadowRadius + (maxImpact - shadowRadius) * position * position;
}

// 冲量映射到纹理列, 与 impactAtColumn 互逆, 供初始化查找和回归测试共用.
export function columnAtImpact(impact: number): number {
  const { minImpact, shadowRadius, maxImpact, width } = blackHoleOptics;
  const position =
    impact < shadowRadius ? -Math.sqrt((shadowRadius - impact) / (shadowRadius - minImpact)) : Math.sqrt((impact - shadowRadius) / (maxImpact - shadowRadius));
  return (position + 1) * 0.5 * (width - 1);
}

// 每场景预计算完整的半精度光轨道表; 返回纹理归场景作用域所有.
export function createBendingTexture(): THREE.DataTexture {
  const { width, height, schwarzschildRadius } = blackHoleOptics;
  const data = new Uint16Array(width * height);
  for (let column = 0; column < width; column++) {
    const impact = impactAtColumn(column);
    const { inverseRadii } = traceLightOrbit(impact, schwarzschildRadius);
    for (let row = 0; row < height; row++) data[row * width + column] = THREE.DataUtils.toHalfFloat(inverseRadii[row] ?? 0);
  }
  const texture = new THREE.DataTexture(data, width, height, THREE.RedFormat, THREE.HalfFloatType);
  texture.minFilter = THREE.LinearFilter;
  texture.magFilter = THREE.LinearFilter;
  texture.needsUpdate = true;
  return texture;
}

// 反求有限距离观察者的入射相位, 避免近景继续沿用无穷远相机; RG16F 同时存储逃逸角.
export function createObserverTexture(): THREE.DataTexture {
  const { width, observerHeight, observerMinRadius, schwarzschildRadius, maxAngle, height } = blackHoleOptics;
  const data = new Uint16Array(width * observerHeight * 2);
  const step = maxAngle / (height - 1);
  for (let column = 0; column < width; column++) {
    const orbit = traceLightOrbit(impactAtColumn(column), schwarzschildRadius);
    for (let row = 0; row < observerHeight; row++) {
      const inverse = row / (observerHeight - 1) / observerMinRadius;
      let index = 1;
      while (index < height - 1 && (orbit.inverseRadii[index] ?? 0) < inverse && (orbit.inverseRadii[index] ?? 0) >= (orbit.inverseRadii[index - 1] ?? 0))
        index++;
      const first = orbit.inverseRadii[index - 1] ?? 0;
      const second = orbit.inverseRadii[index] ?? 0;
      const fraction = THREE.MathUtils.clamp((inverse - first) / Math.max(1e-8, second - first), 0, 1);
      const offset = (row * width + column) * 2;
      data[offset] = THREE.DataUtils.toHalfFloat((index - 1 + fraction) * step);
      data[offset + 1] = THREE.DataUtils.toHalfFloat(orbit.escapeAngle ?? maxAngle);
    }
  }
  const texture = new THREE.DataTexture(data, width, observerHeight, THREE.RGFormat, THREE.HalfFloatType);
  texture.minFilter = THREE.LinearFilter;
  texture.magFilter = THREE.LinearFilter;
  texture.needsUpdate = true;
  return texture;
}

// 反求盘内外缘对应的冲量范围, 供亚像素次像按覆盖面积过滤; RGBA32F 仅用最近采样, 无浮点线性过滤扩展依赖.
export function createImageBoundsTexture(bending: THREE.DataTexture): THREE.DataTexture {
  const { width, height, shadowRadius, maxImpact, maxAngle, discInner, discOuter, discFadeStart } = blackHoleOptics;
  const storage = bending.image.data;
  if (!(storage instanceof Uint16Array)) throw new Error("Invalid bending texture storage");
  const source: Uint16Array = storage;
  const data = new Float32Array(height * 4);
  for (let row = 0; row < height; row++) {
    if ((row / (height - 1)) * maxAngle < Math.PI) continue;
    // 与 GPU 相同的列插值, 同时用于盘缘反求和窄像的发射面积积分.
    function inverseAtImpact(impact: number): number {
      const column = columnAtImpact(impact);
      const left = Math.floor(column);
      const first = THREE.DataUtils.fromHalfFloat(source[row * width + left] ?? 0);
      const second = THREE.DataUtils.fromHalfFloat(source[row * width + Math.min(left + 1, width - 1)] ?? 0);
      return first + (second - first) * (column - left);
    }
    // 轨道表在背面返回路径上对冲量单调, 二分求交只在初始化时执行.
    function findImpact(radius: number): number {
      let lower: number = shadowRadius;
      let upper: number = maxImpact;
      for (let iteration = 0; iteration < 24; iteration++) {
        const impact = (lower + upper) * 0.5;
        const inverse = inverseAtImpact(impact);
        if (inverse > 1 / radius) lower = impact;
        else upper = impact;
      }
      return (lower + upper) * 0.5;
    }
    const inner = findImpact(discInner);
    const outer = findImpact(discOuter);
    // 窄像不能整段使用峰值发射; 对实际冲量区间积分, 保存代表半径和能量修正.
    const flux = (radius: number) => ((discInner * 1.36) / radius) ** 3 * Math.max(0, 1 - Math.sqrt(discInner / radius)) * 7;
    let energy = 0;
    let weightedRadius = 0;
    const samples = 32;
    for (let sample = 0; sample < samples; sample++) {
      const impact = inner + ((outer - inner) * (sample + 0.5)) / samples;
      const radius = THREE.MathUtils.clamp(1 / Math.max(1e-8, inverseAtImpact(impact)), discInner, discOuter);
      const edge = THREE.MathUtils.smoothstep(radius, discInner, discInner + 0.6) * (1 - THREE.MathUtils.smoothstep(radius, discFadeStart, discOuter));
      const emission = flux(radius) * edge;
      energy += emission;
      weightedRadius += radius * emission;
    }
    const radius = energy > 1e-8 ? weightedRadius / energy : discInner;
    data[row * 4] = inner;
    data[row * 4 + 1] = outer;
    data[row * 4 + 2] = radius;
    data[row * 4 + 3] = energy / samples / Math.max(1e-8, flux(radius));
  }
  const texture = new THREE.DataTexture(data, height, 1, THREE.RGBAFormat, THREE.FloatType);
  texture.needsUpdate = true;
  return texture;
}
