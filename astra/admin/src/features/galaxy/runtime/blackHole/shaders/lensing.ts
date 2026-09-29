// 复用逃逸光路映射屏幕背景, 深度用于前后景隔离; 屏外信息和多黑洞相互透镜不在近似范围内.
export const blackHoleLensing = `
  uniform sampler2D backgroundColor;
  uniform sampler2D backgroundDepth;
  uniform vec2 backgroundSize;
  uniform vec2 cameraRange;
  uniform float backgroundEnabled;

  float viewDistance(float depth) {
    return cameraRange.x * cameraRange.y / max(0.0001, cameraRange.y - depth * (cameraRange.y - cameraRange.x));
  }

  // 逃逸方向来自同一光轨道表; 有限背景距离减弱偏移, 避免把前景实体拉入后景.
  vec3 lensBackground(vec3 observer, vec3 tangent, float escapePhase, float impact, float lensDepth) {
    vec2 uv = gl_FragCoord.xy / backgroundSize;
    vec3 original = texture2D(backgroundColor, uv).rgb;
    float backgroundDistance = viewDistance(texture2D(backgroundDepth, uv).r);
    float lensDistance = viewDistance(lensDepth);
    vec3 outgoing = observer * cos(escapePhase) + tangent * sin(escapePhase);
    vec4 direction = projectionMatrix * modelViewMatrix * vec4(outgoing, 0.0);
    // 反向逃逸光线需要屏外环境信息; 本场景使用原背景回退, 不钳制到屏幕边缘拉长像素.
    if (direction.w <= 0.0001) return original;
    vec2 bentUv = direction.xy / direction.w * 0.5 + 0.5;
    float depthWeight = clamp(1.0 - lensDistance / backgroundDistance, 0.0, 1.0);
    float boundary = 1.0 - smoothstep(maxImpact * 0.65, maxImpact, impact);
    vec2 sampleUv = mix(uv, bentUv, depthWeight * boundary);
    vec2 margin = min(sampleUv, 1.0 - sampleUv);
    float screenWeight = smoothstep(0.0, 0.025, min(margin.x, margin.y));
    if (screenWeight <= 0.0) return original;
    float sourceDistance = viewDistance(texture2D(backgroundDepth, sampleUv).r);
    float behind = smoothstep(lensDistance, lensDistance + max(0.5, lensDistance * 0.02), sourceDistance);
    return mix(original, texture2D(backgroundColor, sampleUv).rgb, screenWeight * behind);
  }
`;
