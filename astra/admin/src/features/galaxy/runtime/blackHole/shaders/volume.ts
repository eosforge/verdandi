// 近侧盘缘的有限厚度着色器函数, 补充薄盘在掠射方向的覆盖.
import { blackHoleOptics } from "../config.ts";

// 有限厚度只补充掠射时的近侧盘缘, 弯曲盘像仍由光轨道表负责.
export const blackHoleVolume = `

  // 限定近侧圆柱和垂直截面, 四个固定采样随光程选择 mip, 避免侧视条带闪烁.
  vec4 foregroundRim(vec3 eye, vec3 ray, float observerLapse, float footprint) {
    const float innerRadius = ${blackHoleOptics.discInner.toFixed(8)};
    const float outerRadius = ${blackHoleOptics.discOuter.toFixed(8)};
    const float halfHeight = ${blackHoleOptics.discHalfHeight.toFixed(8)};
    float paddedHeight = halfHeight + min(halfHeight, footprint * 0.5);
    float a = dot(ray.xz, ray.xz);
    float b = dot(eye.xz, ray.xz);
    float discriminant = b * b - a * (dot(eye.xz, eye.xz) - outerRadius * outerRadius);
    if (discriminant <= 0.0 || a < 0.00001) return vec4(0.0);
    float root = sqrt(discriminant);
    float start = max(0.0, (-b - root) / a);
    float end = min((-b + root) / a, -dot(eye, ray));
    if (abs(ray.y) > 0.00001) {
      float first = (-2.0 * paddedHeight - eye.y) / ray.y;
      float second = (2.0 * paddedHeight - eye.y) / ray.y;
      start = max(start, min(first, second));
      end = min(end, max(first, second));
    } else if (abs(eye.y) > 2.0 * paddedHeight) return vec4(0.0);
    if (end <= start) return vec4(0.0);
    float stepLength = (end - start) / 4.0;
    float filterLod = clamp(log2(max(1.0, stepLength * 1024.0 / (outerRadius - innerRadius))), 1.0, 7.0);
    float angularMomentum = cross(eye, ray).y / observerLapse;
    vec3 radiance = vec3(0.0);
    float transmission = 1.0;
    for (int index = 0; index < 4; index++) {
      vec3 point = eye + ray * (start + (float(index) + 0.5) * stepLength);
      float radius = length(point.xz);
      float section = (radius - (innerRadius + outerRadius) * 0.5) / ((outerRadius - innerRadius) * 0.5);
      float height = paddedHeight * sqrt(max(0.0, 1.0 - section * section));
      if (height < 0.00001) continue;
      float density = exp(-2.0 * pow(point.y / height, 2.0)) * halfHeight / paddedHeight;
      vec4 emission = plasmaEmission(point.xz, angularMomentum, observerLapse, filterLod);
      float opacity = 1.0 - exp(-density * emission.a * stepLength * 6.0);
      radiance += transmission * emission.rgb * opacity;
      transmission *= 1.0 - opacity;
    }
    return vec4(radiance / max(1.0 - transmission, 0.001), 1.0 - transmission);
  }
`;
