// 远景星点着色器; 只读观察旋转, 固定远裁剪深度和软边, 不包含动画时钟.
export const distantStarVertex = `
        uniform float pixelRatio;
        attribute vec2 detail;
        varying float brightness;
        void main() {
          // 仅使用观察旋转, 平移和推进不会将遥远星点拉到行星之间.
          vec3 direction = mat3(viewMatrix) * position;
          vec4 clip = projectionMatrix * vec4(direction, 1.0);
          gl_Position = vec4(clip.xy, clip.w, clip.w);
          gl_PointSize = detail.x * pixelRatio;
          brightness = detail.y;
        }
      `;
export const distantStarFragment = `
        uniform vec3 color;
        uniform float strength;
        varying float brightness;
        void main() {
          float radius = length(gl_PointCoord - 0.5);
          if (radius >= 0.5) discard;
          float soft = exp(-radius * radius * 12.0) * (1.0 - smoothstep(0.25, 0.5, radius));
          gl_FragColor = vec4(color * strength * brightness, soft);
          #include <colorspace_fragment>
        }
      `;
