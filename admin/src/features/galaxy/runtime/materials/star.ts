import * as THREE from "three";

// 三维程序纹理随球面旋转, 无贴图接缝; 表面不依赖逐帧 uniform 更新.
export function createStarMaterial(color: string, seed: number) {
  return new THREE.ShaderMaterial({
    uniforms: { color: { value: new THREE.Color(color) }, seed: { value: seed } },
    vertexShader: `
      varying vec3 spherePosition;
      varying vec3 viewNormal;
      varying vec3 viewDirection;
      void main() {
        spherePosition = normalize(position);
        vec4 viewPosition = modelViewMatrix * vec4(position, 1.0);
        viewNormal = normalMatrix * normal;
        viewDirection = -viewPosition.xyz;
        gl_Position = projectionMatrix * viewPosition;
      }
    `,
    fragmentShader: `
      uniform vec3 color;
      uniform float seed;
      varying vec3 spherePosition;
      varying vec3 viewNormal;
      varying vec3 viewDirection;

      float hash(vec3 p) {
        p = fract(p * 0.3183099 + vec3(0.11, 0.27, 0.43));
        p *= 17.0;
        return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
      }
      float noise(vec3 p) {
        vec3 cell = floor(p);
        vec3 f = fract(p);
        f = f * f * (3.0 - 2.0 * f);
        return mix(
          mix(mix(hash(cell), hash(cell + vec3(1,0,0)), f.x),
              mix(hash(cell + vec3(0,1,0)), hash(cell + vec3(1,1,0)), f.x), f.y),
          mix(mix(hash(cell + vec3(0,0,1)), hash(cell + vec3(1,0,1)), f.x),
              mix(hash(cell + vec3(0,1,1)), hash(cell + vec3(1,1,1)), f.x), f.y), f.z);
      }
      float turbulence(vec3 p) {
        float value = 0.0;
        float weight = 0.53;
        for (int i = 0; i < 4; i++) {
          value += noise(p) * weight;
          p = p * 2.03 + vec3(7.1, 3.7, 1.3);
          weight *= 0.5;
        }
        return value;
      }
      void main() {
        vec3 p = normalize(spherePosition) * 5.0 + vec3(seed);
        vec3 warp = vec3(noise(p + 13.1), noise(p + 37.7), noise(p + 71.3));
        vec3 flow = p + (warp - 0.5) * 2.4;
        float convection = turbulence(flow);
        float filaments = 1.0 - abs(noise(flow * 5.0) * 2.0 - 1.0);
        // 远景滤掉比像素更细的颗粒, 避免缩放时出现闪烁和摩尔纹.
        float footprint = length(fwidth(p));
        float detail = 1.0 - smoothstep(0.04, 0.18, footprint);
        float granules = mix(0.5, noise(flow * 17.0), detail);
        float heat = smoothstep(0.22, 0.79, convection + (filaments - 0.5) * 0.22);
        vec3 cool = color * 0.12;
        vec3 warm = pow(color, vec3(0.8)) * 0.95;
        vec3 hot = mix(color, vec3(1.0), 0.48) * 1.35;
        vec3 surface = mix(cool, warm, smoothstep(0.05, 0.65, heat));
        surface = mix(surface, hot, smoothstep(0.58, 0.98, heat));
        surface *= 0.78 + granules * 0.44;
        float facing = clamp(dot(normalize(viewNormal), normalize(viewDirection)), 0.0, 1.0);
        // 恒星自发光, 用临边昏暗表现球体深度, 不制造行星式的背光半球.
        surface *= 0.36 + 0.64 * pow(facing, 0.42);
        surface += color * pow(1.0 - facing, 7.0) * 0.22;
        gl_FragColor = vec4(surface, 1.0);
        #include <tonemapping_fragment>
        #include <colorspace_fragment>
      }
    `,
  });
}

// 日冕使用 GLB 中的球形网格, 依据法线生成柔和亮度, 不包含点精灵或贴图平面.
export function createCoronaMaterial(color: string): THREE.ShaderMaterial {
  return new THREE.ShaderMaterial({
    uniforms: { color: { value: new THREE.Color(color) } },
    vertexShader: `
      varying vec3 viewNormal;
      varying vec3 viewDirection;
      varying vec3 localDirection;
      // 保留模型表面坐标, 日冕纹理不会因相机转动而平移.
      void main() {
        vec4 viewPosition = modelViewMatrix * vec4(position, 1.0);
        viewNormal = normalMatrix * normal;
        viewDirection = -viewPosition.xyz;
        localDirection = normalize(position);
        gl_Position = projectionMatrix * viewPosition;
      }
    `,
    fragmentShader: `
      uniform vec3 color;
      varying vec3 viewNormal;
      varying vec3 viewDirection;
      varying vec3 localDirection;
      // 轮廓内外均渐隐, 保留球面表面细节, 不生成规则亮圆框.
      void main() {
        float facing = max(0.0, dot(normalize(viewNormal), normalize(viewDirection)));
        float wisps = 0.7 + 0.3 * sin(localDirection.x * 27.0 + sin(localDirection.y * 19.0) + localDirection.z * 13.0);
        float alpha = exp(-pow((facing - 0.70) / 0.18, 2.0)) * (1.0 - smoothstep(0.78, 0.96, facing)) * wisps * 0.28;
        if (alpha < 0.002) discard;
        gl_FragColor = vec4(color * 1.3, alpha);
        #include <tonemapping_fragment>
        #include <colorspace_fragment>
      }
    `,
    transparent: true,
    blending: THREE.AdditiveBlending,
    depthWrite: false,
  });
}
