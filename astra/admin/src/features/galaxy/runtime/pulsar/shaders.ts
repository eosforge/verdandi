// 脉冲星表面、稀薄光晕、偶极场与双极辐射体积的 GLSL; 只提供着色源码.
export const surfaceVertex = `
  uniform vec3 magneticAxis;
  varying vec3 localDirection;
  varying vec3 viewNormal;
  varying vec3 viewDirection;
  varying float pulse;
  void main() {
    vec4 viewPosition = modelViewMatrix * vec4(position, 1.0);
    localDirection = normalize(position);
    viewNormal = normalMatrix * normal;
    viewDirection = -viewPosition.xyz;
    vec3 center = (modelMatrix * vec4(0.0, 0.0, 0.0, 1.0)).xyz;
    vec3 axis = normalize(mat3(modelMatrix) * magneticAxis);
    float alignment = abs(dot(normalize(cameraPosition - center), axis));
    // 只有视线进入磁极光束时增强脉冲, 不使用统一的时间正弦闪烁.
    pulse = smoothstep(0.93969, 0.99255, alignment);
    gl_Position = projectionMatrix * viewPosition;
  }
`;

export const surfaceFragment = `
  uniform vec3 magneticAxis;
  uniform float phase;
  varying vec3 localDirection;
  varying vec3 viewNormal;
  varying vec3 viewDirection;
  varying float pulse;
  void main() {
    vec3 p = normalize(localDirection);
    float facing = max(0.0, dot(normalize(viewNormal), normalize(viewDirection)));
    float cap = pow(abs(dot(p, magneticAxis)), 18.0);
    // 弱纹理沿磁纬分布, 两个热极更亮; 不以大幅规则条纹覆盖整颗核心.
    float latitude = dot(p, magneticAxis);
    float grain = sin(latitude * 43.0 + sin(p.z * 18.0 + sin(phase) * 0.3)) * sin(p.x * 29.0 + p.y * 17.0);
    float filterWeight = 1.0 - smoothstep(0.04, 0.15, length(fwidth(p)));
    vec3 cool = vec3(0.09, 0.23, 0.45);
    vec3 hot = vec3(1.0, 1.08, 1.16);
    vec3 emission = mix(cool, hot, pow(facing, 0.36));
    emission *= 0.97 + 0.03 * grain * filterWeight;
    emission += vec3(0.9, 0.86, 0.8) * cap;
    emission += vec3(0.38, 0.45, 0.52) * pulse * (0.3 + 0.7 * facing);
    gl_FragColor = vec4(emission, 1.0);
    #include <tonemapping_fragment>
    #include <colorspace_fragment>
  }
`;

export const haloFragment = `
  uniform vec3 localCamera;
  uniform vec3 magneticAxis;
  uniform float coreRadius;
  uniform float haloRadius;
  varying vec3 localPosition;
  // 有限光路上的高斯发射积分, 让辉光从核心连续扩散, 不显示球壳轮廓.
  float erfApprox(float value) {
    float t = 1.0 / (1.0 + 0.3275911 * abs(value));
    float polynomial = (((((1.061405429 * t - 1.453152027) * t) + 1.421413741) * t - 0.284496736) * t + 0.254829592) * t;
    return sign(value) * (1.0 - polynomial * exp(-value * value));
  }
  float integrateGlow(float width, float impactSquared, float start, float end) {
    return exp(-impactSquared / (2.0 * width * width)) * width * 1.253314
      * (erfApprox(end / (1.414214 * width)) - erfApprox(start / (1.414214 * width))) / coreRadius;
  }
  void main() {
    vec3 direction = normalize(localPosition - localCamera);
    float midpoint = -dot(localCamera, direction);
    float impactSquared = max(0.0, dot(localCamera, localCamera) - midpoint * midpoint);
    float halfChord = sqrt(max(0.0, haloRadius * haloRadius - impactSquared));
    float start = max(0.0, midpoint - halfChord) - midpoint;
    float end = halfChord;
    if (end <= start) discard;
    float warm = integrateGlow(coreRadius * 0.8, impactSquared, start, end);
    float blue = integrateGlow(coreRadius * 1.5, impactSquared, start, end);
    float edge = 1.0 - smoothstep(haloRadius * 0.68, haloRadius, sqrt(impactSquared));
    float pulse = smoothstep(0.93969, 0.99255, abs(dot(normalize(localCamera), magneticAxis)));
    vec3 emission = (vec3(1.0, 0.88, 0.78) * warm * 0.48 + vec3(0.08, 0.32, 0.95) * blue * 0.1) * edge * (1.0 + pulse * 0.4);
    if (max(emission.r, max(emission.g, emission.b)) < 0.001) discard;
    gl_FragColor = vec4(emission, 1.0);
    #include <tonemapping_fragment>
    #include <colorspace_fragment>
  }
`;

export const beamVertex = `
  varying vec3 localPosition;
  void main() {
    localPosition = position;
    gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0);
  }
`;

export const beamFragment = `
  uniform vec3 localCamera;
  uniform float beamLength;
  uniform float beamRadius;
  uniform float coreRadius;
  uniform float phase;
  varying vec3 localPosition;
  void main() {
    vec3 direction = normalize(localPosition - localCamera);
    // 封闭圆柱只作为体积包围网格; 光束密度在边界前归零, 不显示圆柱外壳.
    vec3 safeDirection = mix(vec3(-1.0), vec3(1.0), step(vec3(0.0), direction)) * max(abs(direction), vec3(0.00001));
    vec3 entry = (vec3(-beamRadius, 0.0, -beamRadius) - localCamera) / safeDirection;
    vec3 exitPoint = (vec3(beamRadius, beamLength, beamRadius) - localCamera) / safeDirection;
    vec3 lower = min(entry, exitPoint);
    vec3 upper = max(entry, exitPoint);
    float nearPoint = max(0.0, max(lower.x, max(lower.y, lower.z)));
    float farPoint = min(upper.x, min(upper.y, upper.z));
    if (farPoint <= nearPoint) discard;
    float stepLength = (farPoint - nearPoint) / 64.0;
    vec3 light = vec3(0.0);
    float transmission = 1.0;
    for (int sampleIndex = 0; sampleIndex < 64; sampleIndex++) {
      vec3 p = localCamera + direction * (nearPoint + (float(sampleIndex) + 0.5) * stepLength);
      float axial = max(0.0, p.y - coreRadius);
      float width = 0.38 + axial * 0.11;
      float radial = length(p.xz) / width;
      float center = exp(-radial * radial * 12.0);
      float sheath = exp(-radial * radial * 2.2);
      float filaments = 0.0;
      // 不同相位的细丝沿光束向外传输, 光柱保留细亮芯和较疏松的蓝色外层.
      for (int strand = 0; strand < 5; strand++) {
        float seed = float(strand);
        float angle = seed * 2.39996 + axial * 0.035 + sin(phase + seed) * 0.14;
        vec2 offset = vec2(cos(angle), sin(angle)) * width * (0.42 + 0.08 * sin(seed * 3.0));
        float filamentRadius = max(0.055, width * 0.085);
        float distanceToStrand = length(p.xz - offset) / filamentRadius;
        float packet = pow(0.5 + 0.5 * cos(axial * 0.65 - phase * 3.0 + seed * 1.7), 4.0);
        filaments += exp(-distanceToStrand * distanceToStrand * 1.6) * (0.32 + packet * 1.25);
      }
      float envelope = smoothstep(coreRadius * 0.94, coreRadius * 1.3, p.y)
        * (1.0 - smoothstep(beamLength * 0.45, beamLength, p.y))
        * (1.0 - smoothstep(beamRadius * 0.78, beamRadius, length(p.xz)));
      float strands = 0.6 + 0.4 * sin(atan(p.z, p.x + 0.00001) * 7.0 + axial * 0.3 - phase);
      float density = envelope * (center * 0.9 + filaments * 0.85 + sheath * 0.12 * strands) / (1.0 + axial * 0.025);
      float alpha = 1.0 - exp(-density * stepLength);
      vec3 emission = mix(vec3(0.045, 0.2, 0.95), vec3(1.12, 1.16, 1.22), clamp(center + filaments * 0.65, 0.0, 1.0));
      light += transmission * alpha * emission;
      transmission *= 1.0 - alpha;
    }
    if (1.0 - transmission < 0.002) discard;
    gl_FragColor = vec4(light, 1.0 - transmission);
    #include <tonemapping_fragment>
    #include <colorspace_fragment>
  }
`;

export const fieldVertex = `
  attribute float fieldSeed;
  varying float visibility;
  varying vec2 fieldUv;
  varying float seed;
  varying vec3 viewNormal;
  varying vec3 viewDirection;
  void main() {
    vec4 center = modelViewMatrix * vec4(0.0, 0.0, 0.0, 1.0);
    float scale = length(modelMatrix[0].xyz);
    float apparentSize = projectionMatrix[1][1] * scale / max(0.01, -center.z);
    visibility = smoothstep(0.0015, 0.009, apparentSize);
    fieldUv = uv;
    seed = fieldSeed;
    vec4 viewPosition = modelViewMatrix * vec4(position, 1.0);
    viewNormal = normalMatrix * normal;
    viewDirection = -viewPosition.xyz;
    gl_Position = projectionMatrix * viewPosition;
  }
`;

export const fieldFragment = `
  uniform float phase;
  uniform float glowLayer;
  varying float visibility;
  varying vec2 fieldUv;
  varying float seed;
  varying vec3 viewNormal;
  varying vec3 viewDirection;
  void main() {
    if (visibility < 0.01) discard;
    float pulse = pow(0.5 + 0.5 * cos(fieldUv.x * 19.0 - phase * 2.0 + seed * 2.1), 7.0);
    float filament = 0.6 + 0.4 * sin(fieldUv.x * 47.0 + seed * 5.3 + sin(phase));
    float ends = smoothstep(0.0, 0.06, fieldUv.x) * (1.0 - smoothstep(0.94, 1.0, fieldUv.x));
    float facing = max(0.0, dot(normalize(viewNormal), normalize(viewDirection)));
    float profile = mix(0.65 + facing * 0.35, pow(facing, 3.0), glowLayer);
    float alpha = visibility * ends * profile * mix(0.36, 0.11, glowLayer) * (0.4 + filament * 0.35 + pulse * 1.25);
    vec3 color = mix(vec3(0.06, 0.22, 0.8), vec3(0.42, 0.8, 1.0), pulse * 0.75 + filament * 0.15);
    gl_FragColor = vec4(color, alpha);
    #include <tonemapping_fragment>
    #include <colorspace_fragment>
  }
`;
