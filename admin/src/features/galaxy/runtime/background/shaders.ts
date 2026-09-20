// 背景辐射的艺术化可视化, 非真实微波观测数据; 仅输出天空颜色, 不给星体增加照明.
export const cosmicBackgroundVertex = `
  uniform mat4 inverseProjection;
  uniform mat4 cameraWorld;
  varying vec3 skyDirection;
  void main() {
    vec4 view = inverseProjection * vec4(position.xy, 1.0, 1.0);
    skyDirection = mat3(cameraWorld) * view.xyz;
    // 天空置于远裁剪面, 深度缓冲仍保持清屏值.
    gl_Position = vec4(position.xy, 1.0, 1.0);
  }
`;

export const cosmicBackgroundFragment = `
  uniform vec3 baseColor;
  uniform vec3 coolColor;
  uniform vec3 warmColor;
  uniform float radiationStrength;
  uniform float grainStrength;
  varying vec3 skyDirection;

  float hash(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
  }
  float noise(vec3 p) {
    vec3 cell = floor(p);
    vec3 f = fract(p);
    f = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    return mix(
      mix(mix(hash(cell), hash(cell + vec3(1,0,0)), f.x),
          mix(hash(cell + vec3(0,1,0)), hash(cell + vec3(1,1,0)), f.x), f.y),
      mix(mix(hash(cell + vec3(0,0,1)), hash(cell + vec3(1,0,1)), f.x),
          mix(hash(cell + vec3(0,1,1)), hash(cell + vec3(1,1,1)), f.x), f.y), f.z);
  }
  void main() {
    vec3 direction = normalize(skyDirection);
    // 世界方向定义无接缝低频起伏; 平移/推近不产生贴在镜头前的云层视差.
    vec3 p = direction * 2.4 + vec3(12.3, 5.7, 19.1);
    float broad = noise(p);
    float detail = noise(p * 2.13 + vec3(3.7, 9.1, 1.3));
    float field = broad * 0.9 + detail * 0.1;
    float cloud = smoothstep(0.1, 0.95, field);
    vec3 radiation = mix(coolColor, warmColor, smoothstep(0.5, 0.85, broad));
    // 固定像素细粒而非逐帧随机闪烁; 只作用于背景, 不叠加到星体表面.
    float grain = hash(vec3(floor(gl_FragCoord.xy), 17.0)) - 0.5;
    vec3 color = baseColor + radiation * cloud * radiationStrength;
    color += grain * grainStrength;
    gl_FragColor = vec4(max(color, vec3(0.0)), 1.0);
    #include <colorspace_fragment>
  }
`;
