import { blackHoleOptics } from "./blackHoleOptics.ts";

// 所有光线交点共用同一发射函数; 物理盘半径与屏幕上的临界阴影半径是不同量.
export const blackHolePlasma = `
  uniform sampler2D plasmaMap;
  uniform float flowTime;
  uniform float detailLevel;
  uniform vec4 brightKnots[3];
  // 体积采样使用显式 LOD, 避免稀疏相交分支中的屏幕导数; 盘像保留周期接缝修正.
  vec3 readPlasma(vec2 uv, float filterLod) {
    if (filterLod >= 0.0) return textureLod(plasmaMap, uv, max(filterLod, mix(2.0, 1.0, detailLevel))).rgb;
    vec2 dx = dFdx(uv);
    vec2 dy = dFdy(uv);
    dx.x -= floor(dx.x + 0.5);
    dy.x -= floor(dy.x + 0.5);
    float mipScale = mix(1.15, 1.0, detailLevel);
    return textureGrad(plasmaMap, uv, dx * mipScale, dy * mipScale).rgb;
  }
  // 两个错开半周期的流场在零权重处复位, 防止径向差速持续累积成无限细纹或周期跳变.
  vec3 flowingPlasma(float angle, float radius, float filterLod) {
    float speed = 0.036 + 0.21 * pow(4.5 / max(radius, 4.5), 1.5);
    float phase = flowTime / 48.0;
    float otherPhase = fract(phase + 0.5);
    float blend = sin(phase * 3.14159265);
    blend *= blend;
    float radial = (radius - ${blackHoleOptics.discInner.toFixed(8)}) / ${(blackHoleOptics.discOuter - blackHoleOptics.discInner).toFixed(8)};
    vec3 first = readPlasma(vec2((angle - phase * 48.0 * speed) / 6.2831853, radial), filterLod);
    vec3 second = readPlasma(vec2((angle - otherPhase * 48.0 * speed) / 6.2831853, radial), filterLod);
    return mix(second, first, blend);
  }
  // 返回线性发射颜色和覆盖率; 发热峰在盘内缘之外, 避免只给阴影描一道亮边.
  vec4 plasmaEmission(vec2 point, float angularMomentum, float observerLapse, float filterLod) {
    float radius = length(point);
    float angle = atan(point.y, point.x);
    const float innerRadius = ${blackHoleOptics.discInner.toFixed(8)};
    const float outerRadius = ${blackHoleOptics.discOuter.toFixed(8)};
    float edge = smoothstep(innerRadius, innerRadius + 0.6, radius)
      * (1.0 - smoothstep(${blackHoleOptics.discFadeStart.toFixed(8)}, outerRadius, radius));
    float flux = pow(innerRadius * 1.36 / max(radius, innerRadius), 3.0) * max(0.0, 1.0 - sqrt(innerRadius / max(radius, innerRadius))) * 7.0;
    vec3 textureValue = flowingPlasma(angle, radius, filterLod);
    float knots = 0.0;
    if (filterLod < 4.0) {
      for (int index = 0; index < 3; index++) {
        vec4 knot = brightKnots[index];
        float da = (fract((angle - knot.x) / 6.2831853 + 0.5) - 0.5) * 6.2831853 / max(0.05, knot.w);
        float dr = (radius - knot.y) / mix(1.2, 0.32, detailLevel);
        knots += exp(-0.5 * (da * da + dr * dr)) * knot.z;
      }
    }
    // 远景混合分段流团和过滤后的细丝能量, 不再仅靠宽亮带; 不对低密度尾部作提亮以免形成塑料高光.
    float fineFilament = 0.012 + pow(textureValue.r, 1.5) * 4.5;
    float broadFlow = 0.025 + textureValue.b * 5.0 + pow(textureValue.r, 1.5) * 1.8;
    float filament = mix(broadFlow, fineFilament, detailLevel) + knots * 0.55;
    vec3 warm = mix(vec3(0.76, 0.23, 0.065), vec3(1.0, 0.82, 0.6), clamp(flux, 0.0, 1.0));
    // 圆轨道发射体的频移使用光路守恒角动量, 不以光源到相机的直线方向替代弯曲光线.
    float orbitRadius = max(radius, innerRadius);
    float frequency = sqrt(${(blackHoleOptics.schwarzschildRadius * 0.5).toFixed(8)} / (orbitRadius * orbitRadius * orbitRadius));
    float lapse = sqrt(1.0 - ${(blackHoleOptics.schwarzschildRadius * 1.5).toFixed(8)} / orbitRadius);
    float shift = clamp(lapse / (observerLapse * max(0.2, 1.0 - frequency * angularMomentum)), 0.3, 2.0);
    float temperature = clamp(flux * (0.72 + textureValue.g * 0.18 + textureValue.r * 0.16 + knots * 0.12) * shift, 0.0, 1.0);
    // 内部高温流采用暖白色, 提升发射能量而保留密度间隙, 外层只保留低饱和琥珀色.
    float heat = smoothstep(0.18, 0.85, temperature);
    vec3 color = mix(warm, vec3(1.0, 0.97, 0.9), heat);
    float beaming = shift * shift * shift;
    vec3 emission = color * flux * filament * beaming * 2.15 * (1.0 + heat * 0.45);
    return vec4(emission, edge);
  }
  // 局部曝光压缩保留暖白中心和琥珀外缘, 不改变整个星图的曝光.
  vec3 exposePlasma(vec3 emission) {
    return vec3(1.0) - exp(-emission * 1.25);
  }
`;
