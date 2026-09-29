// 纯数值光轨道和冲量映射, 不创建 Three.js 或 DOM 资源.
import { blackHoleOptics } from "./config.ts";

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
