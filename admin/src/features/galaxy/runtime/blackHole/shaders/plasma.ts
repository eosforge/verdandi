// 吸积盘发射、频移与螺旋平流着色器函数, 供所有光路交点复用.
import { blackHoleOptics } from "../config.ts";
import { blackHoleFlow } from "../flow.ts";

// 所有光线交点共用同一发射函数; 物理盘半径与屏幕上的临界阴影半径是不同量.
export const blackHolePlasma = `
  uniform sampler2D plasmaMap;
  uniform float flowTime;
  uniform float detailLevel;
  uniform vec4 brightKnots[3];
  // 体积采样使用显式 LOD, 避免稀疏相交分支中的屏幕导数; 盘像保留周期接缝修正.
  vec4 readPlasma(vec2 uv, float filterLod) {
    if (filterLod >= 0.0) return textureLod(plasmaMap, uv, max(filterLod, mix(1.0, 0.0, detailLevel)));
    vec2 dx = dFdx(uv);
    vec2 dy = dFdy(uv);
    dx.x -= floor(dx.x + 0.5);
    dy.x -= floor(dy.x + 0.5);
    float mipScale = mix(1.15, 1.0, detailLevel);
    return textureGrad(plasmaMap, uv, dx * mipScale, dy * mipScale);
  }
  // 反向追溯 dr/dt = -v_in sqrt(r_in/r) 的纹理来源, 再沿这条螺旋积分角位移.
  // 正向观察时细丝持续收缩, 不是仅在固定半径上旋转或叠加全盘呼吸缩放.
  vec2 flowOrigin(float angle, float radius, float age, float angularRatio, float inflowRatio) {
    const float inner = ${blackHoleOptics.discInner.toFixed(8)};
    float current = max(radius, inner);
    float inwardSpeed = ${blackHoleFlow.innerRadialSpeed.toFixed(8)} * inflowRatio;
    float sourceRadius = pow(pow(current, 1.5) + 1.5 * inwardSpeed * sqrt(inner) * age, 2.0 / 3.0);
    float turn = ${blackHoleFlow.innerAngularSpeed.toFixed(8)} * angularRatio * inner / inwardSpeed * log(sourceRadius / current);
    return vec2((angle - turn) / 6.2831853, (sourceRadius - inner) / ${(blackHoleOptics.discOuter - blackHoleOptics.discInner).toFixed(8)});
  }
  // 双相复位仍发生在该相权重为零时; 两个层次共用内流形式, 各自具有差速.
  vec4 advectPlasma(float angle, float radius, float filterLod, float angularRatio, float inflowRatio) {
    float phase = flowTime / ${blackHoleFlow.cycleSeconds.toFixed(1)};
    float otherPhase = fract(phase + 0.5);
    float blend = sin(phase * 3.14159265);
    blend *= blend;
    vec4 first = readPlasma(flowOrigin(angle, radius, phase * ${blackHoleFlow.cycleSeconds.toFixed(1)}, angularRatio, inflowRatio), filterLod);
    vec4 second = readPlasma(flowOrigin(angle, radius, otherPhase * ${blackHoleFlow.cycleSeconds.toFixed(1)}, angularRatio, inflowRatio), filterLod);
    return mix(second, first, blend);
  }
  // 内快外慢的主流加上缓慢漂移的稀薄层; 层次通过独立密度与透射表现, 不增加几何网格.
  vec4 flowingPlasma(float angle, float radius, float filterLod) {
    vec4 mainFlow = advectPlasma(angle, radius, filterLod, 1.0, 1.0);
    // 散射和极粗体积采样只需平均能量, 不额外采样已不可分辨的独立薄雾运动.
    if (filterLod >= 4.0) return mainFlow;
    vec4 atmosphere = advectPlasma(angle + 1.7, radius, filterLod, ${blackHoleFlow.atmosphereSpeedRatio.toFixed(8)}, ${blackHoleFlow.atmosphereInflowRatio.toFixed(8)});
    return vec4(mainFlow.rgb, atmosphere.a);
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
    vec4 textureValue = flowingPlasma(angle, radius, filterLod);
    float radial = clamp((radius - innerRadius) / (outerRadius - innerRadius), 0.0, 1.0);
    float outerFlow = smoothstep(0.12, 0.75, radial);
    // 收拢主发光带, 峰值留在盘内缘之外, 不给捕获阴影画固定亮圈.
    float coreBand = exp(-0.5 * pow((radius - innerRadius * 1.36) / (innerRadius * 0.19), 2.0));
    float concentratedFlux = flux * (0.12 + coreBand * 1.7);
    // 外层按随流场移动的密度提前散开, 最外边界仍被光学体积覆盖并平滑归零.
    float tailStart = mix(outerRadius * 0.52, outerRadius * 0.78, textureValue.g);
    float tailEnd = mix(outerRadius * 0.86, outerRadius, textureValue.a);
    edge *= 1.0 - smoothstep(tailStart, tailEnd, radius);
    float knots = 0.0;
    if (filterLod < 4.0) {
      for (int index = 0; index < 3; index++) {
        vec4 knot = brightKnots[index];
        float da = (fract((angle - knot.x) / 6.2831853 + 0.5) - 0.5) * 6.2831853 / max(0.05, knot.w);
        float dr = (radius - knot.y) / mix(1.2, 0.32, detailLevel);
        knots += exp(-0.5 * (da * da + dr * dr)) * knot.z;
      }
    }
    // 内部由连续发光层承载主亮度, 细丝只调制流动; 外盘保持低能量, 避免每根纹理都过曝成白线.
    float fineFilament = textureValue.r * 1.65 + textureValue.b * 0.2;
    float broadFlow = textureValue.b * 0.7 + textureValue.r * 0.85;
    float luminousLayer = coreBand * (0.60 + textureValue.g * 0.12);
    float filament = 0.008 + luminousLayer + mix(broadFlow, fineFilament, detailLevel) + textureValue.a * 0.08 + knots * 0.24;
    vec3 warm = mix(vec3(0.65, 0.16, 0.024), vec3(1.0, 0.64, 0.19), clamp(flux, 0.0, 1.0));
    // 圆轨道发射体的频移使用光路守恒角动量, 不以光源到相机的直线方向替代弯曲光线.
    float orbitRadius = max(radius, innerRadius);
    float frequency = sqrt(${(blackHoleOptics.schwarzschildRadius * 0.5).toFixed(8)} / (orbitRadius * orbitRadius * orbitRadius));
    float lapse = sqrt(1.0 - ${(blackHoleOptics.schwarzschildRadius * 1.5).toFixed(8)} / orbitRadius);
    float shift = clamp(lapse / (observerLapse * max(0.2, 1.0 - frequency * angularMomentum)), 0.3, 2.0);
    float temperature = clamp(concentratedFlux * (0.72 + textureValue.g * 0.18 + textureValue.r * 0.16 + knots * 0.12) * shift, 0.0, 1.0);
    // 参考图采用暖金色主亮环, 保留亮度层次, 不把整段内盘推成无色白带.
    float heat = smoothstep(0.18, 0.85, temperature);
    vec3 color = mix(warm, vec3(1.0, 0.84, 0.48), heat);
    float beaming = shift * shift * shift;
    float thinWisps = pow(textureValue.a, 1.4) * outerFlow * 0.07;
    vec3 emission = color * (concentratedFlux * filament + thinWisps) * beaming * 2.15 * (1.0 + heat * 0.45);
    // 主体有遮挡, 外盘低密度处可透光, 避免外缘成为一块不透明的圆盘表面.
    float density = 0.12 + textureValue.g * 0.9 + textureValue.r * 0.8 + textureValue.a * 0.18;
    float opticalDepth = density * mix(4.2, 0.07, pow(radial, 0.4));
    return vec4(emission, edge * (1.0 - exp(-opticalDepth)));
  }
  // 压缩高光但保留暖金色; 仅最高能量略微趋白, 不把密集流丝压成银白色同心线.
  vec3 exposePlasma(vec3 emission) {
    float luminance = dot(emission, vec3(0.2126, 0.7152, 0.0722));
    vec3 chroma = emission / max(luminance, 0.00001);
    float whiteHeat = smoothstep(4.0, 16.0, luminance) * 0.18;
    return mix(chroma, vec3(1.0), whiteHeat) * (luminance / (0.85 + luminance));
  }
`;
