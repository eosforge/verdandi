// 光学体积的着色器装配; 盘发射、厚度和背景透镜各自维护独立函数块.
import { blackHoleOptics as optics } from "../config.ts";
import { blackHolePlasma } from "./plasma.ts";
import { blackHoleVolume } from "./volume.ts";
import { blackHoleLensing } from "./lensing.ts";

export const horizonVertexShader = `
      varying vec3 localPosition;
      // 球壳只包围光学体积, 不作为发光表面, 也不朝向相机旋转.
      void main() {
        localPosition = position;
        gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0);
      }
    `;

export const horizonFragmentShader = `
      uniform mat4 projectionMatrix;
      uniform mat4 modelViewMatrix;
      uniform vec3 discNormal;
      uniform mat3 discBasis;
      uniform sampler2D bending;
      uniform sampler2D imageBounds;
      uniform sampler2D observerTable;
      const float shadowRadius = ${optics.shadowRadius.toFixed(8)};
      const float minImpact = ${optics.minImpact.toFixed(8)};
      const float maxImpact = ${optics.maxImpact.toFixed(8)};
      const float volumeRadius = ${optics.volumeRadius.toFixed(8)};
      const float gravityRadius = ${optics.schwarzschildRadius.toFixed(8)};
      const float maxAngle = ${optics.maxAngle.toFixed(8)};
      const vec2 tableSize = vec2(${optics.width.toFixed(1)}, ${optics.height.toFixed(1)});
      varying vec3 localPosition;
      uniform vec3 localEye;
      ${blackHolePlasma}
      ${blackHoleVolume}
      ${blackHoleLensing}
      // 临界值两侧使用相同的非线性采样, 避免把捕获区当作普通黑球的投影.
      float impactColumn(float impact) {
        float offset = impact < shadowRadius
          ? -sqrt(clamp((shadowRadius - impact) / (shadowRadius - minImpact), 0.0, 1.0))
          : sqrt(clamp((impact - shadowRadius) / (maxImpact - shadowRadius), 0.0, 1.0));
        return (offset + 1.0) * 0.5;
      }
      // 真实盘面交点的温度、纹理与旋转速度保持一致, 不为上下盘像分别画光弧.
      vec4 discImage(float column, float phase, float cameraPhase, vec3 observer, vec3 tangent, float angularMomentum, float observerLapse, float lod) {
        vec2 uv = (vec2(column, (phase + cameraPhase) / maxAngle) * (tableSize - 1.0) + 0.5) / tableSize;
        float inverseRadius = texture2D(bending, uv).r;
        float radius = 1.0 / max(inverseRadius, 0.0001);
        vec3 direction = observer * cos(phase) + tangent * sin(phase);
        vec3 source = discBasis * direction * radius;
        vec4 emission = plasmaEmission(source.xz, angularMomentum, observerLapse, lod);
        // 用局部直线倾角近似盘层光程, 掠射更致密, 俯视可见稀疏外盘; 非完整体积转移.
        float pathCosine = abs(dot(discNormal, normalize(direction * radius - localEye)));
        emission.a = 1.0 - pow(max(0.0, 1.0 - emission.a), 1.0 / max(0.18, pathCosine));
        emission.a *= step(phase + cameraPhase, maxAngle);
        return emission;
      }
      // 只对不足两像素的盘像使用覆盖近似; 可分辨盘像必须保留真实半径、发热梯度和盘缘.
      vec4 filteredImage(float impact, float phase, float cameraPhase, float footprint, vec3 observer, vec3 tangent,
                         float angularMomentum, float observerLapse, out float filterWeight) {
        float row = clamp((phase + cameraPhase) / maxAngle, 0.0, 1.0) * (tableSize.y - 1.0);
        vec4 first = texture2D(imageBounds, vec2((floor(row) + 0.5) / tableSize.y, 0.5));
        vec4 second = texture2D(imageBounds, vec2((min(floor(row) + 1.0, tableSize.y - 1.0) + 0.5) / tableSize.y, 0.5));
        vec4 bounds = mix(first, second, fract(row));
        float pixels = max(0.0, bounds.y - bounds.x) / (2.0 * footprint);
        filterWeight = 1.0 - smoothstep(1.0, 2.0, pixels);
        float overlap = max(0.0, min(impact + footprint, bounds.y) - max(impact - footprint, bounds.x));
        float coverage = overlap / (2.0 * footprint) * step(phase + cameraPhase, maxAngle);
        vec3 source = discBasis * (observer * cos(phase) + tangent * sin(phase)) * max(bounds.z, ${optics.discInner.toFixed(8)});
        vec4 emission = plasmaEmission(source.xz, angularMomentum, observerLapse, 5.0);
        float pathCosine = abs(normalize(source - discBasis * localEye).y);
        float opacity = 1.0 - pow(max(0.0, 1.0 - emission.a), 1.0 / max(0.18, pathCosine));
        return vec4(emission.rgb * bounds.w, coverage * opacity);
      }
      // 按沿光路的先后顺序遮挡, 先遇到的不透明盘面会挡住后续盘像和阴影.
      void accumulate(vec4 sampleValue, inout vec3 radiance, inout float transmission) {
        radiance += transmission * sampleValue.rgb * sampleValue.a;
        transmission *= 1.0 - sampleValue.a;
      }
      // 经授权的微弱弧面表现, 并非事件视界发射或反射; 法线留在模型坐标, 转动相机会改变明暗.
      vec3 coreRelief(vec3 ray, float impact, float observerLapse) {
        float relativeImpact = clamp(impact / shadowRadius, 0.0, 1.0);
        float facing = sqrt(max(0.0, 1.0 - relativeImpact * relativeImpact));
        float apparentRadius = shadowRadius * observerLapse;
        vec3 surface = localEye + ray * (-dot(localEye, ray) - apparentRadius * facing);
        vec3 normal = normalize(discBasis * surface);
        float illumination = max(0.0, dot(normal, normalize(vec3(-0.55, 0.7, 0.45))));
        float rim = pow(1.0 - facing, 2.2);
        float equator = pow(1.0 - abs(normal.y), 3.0);
        // 暗部渐变只提示弧度, 盘面附近的暖色轮廓保持低亮度, 避免整圈描边或金属高光.
        vec3 body = vec3(0.0045, 0.0040, 0.0048) * pow(illumination, 1.5);
        vec3 warmRim = vec3(0.022, 0.009, 0.0035) * rim * equator * (0.2 + 0.8 * illumination);
        vec3 coolRim = vec3(0.003, 0.0045, 0.007) * rim * max(0.0, normal.y);
        return body + warmRim + coolRim;
      }
      // 每像素查询固定数量的交点; 阴影由光线捕获决定, 不再叠加一个球体表面.
      void main() {
        float eyeDistance = length(localEye);
        if (gl_FrontFacing == (eyeDistance < volumeRadius)) discard;
        vec3 observer = localEye / eyeDistance;
        vec3 ray = normalize(localPosition - localEye);
        float radialDirection = dot(observer, ray);
        float observerLapse = sqrt(max(0.1, 1.0 - gravityRadius / eyeDistance));
        float impact = length(cross(localEye, ray)) / observerLapse;
        if (impact > maxImpact) discard;
        float column = impactColumn(impact);
        float footprint = max(0.012, fwidth(impact) * 0.65);
        vec2 observerSize = vec2(tableSize.x, ${optics.observerHeight.toFixed(1)});
        float observerRow = clamp(${optics.observerMinRadius.toFixed(1)} / eyeDistance, 0.0, 1.0);
        vec2 observerUv = (vec2(column, observerRow) * (observerSize - 1.0) + 0.5) / observerSize;
        vec2 angles = texture2D(observerTable, observerUv).rg;
        float cameraPhase = radialDirection < 0.0 ? angles.r : angles.g - angles.r;
        vec3 tangential = ray - observer * radialDirection;
        vec3 tangent = tangential / max(length(tangential), 0.00001);
        // 当前盘流沿 atan(z,x) 增大的方向运动, 守恒角动量按同一手性约定投影到盘轴.
        float angularMomentum = impact * dot(discNormal, cross(observer, tangent));
        // 精确侧视时消除插值舍入产生的倾角正负噪声, 避免相邻像素在零与半圈交点之间交替.
        float inclination = dot(discNormal, observer);
        if (abs(inclination) < 0.000001) inclination = 0.0;
        float phase = mod(atan(-inclination, dot(discNormal, tangent)) + 3.14159265, 3.14159265);
        vec4 primary = discImage(column, phase, cameraPhase, observer, tangent, angularMomentum, observerLapse, -1.0);
        float filterWeight;
        vec4 filtered = filteredImage(impact, phase + 3.14159265, cameraPhase, footprint, observer, tangent, angularMomentum, observerLapse, filterWeight);
        vec4 secondary = discImage(column, phase + 3.14159265, cameraPhase, observer, tangent, angularMomentum, observerLapse, -1.0);
        // 以覆盖率加权颜色, 防止透明盘缘混入近似像的白热颜色而形成硬边.
        secondary = mix(vec4(secondary.rgb * secondary.a, secondary.a), vec4(filtered.rgb * filtered.a, filtered.a), filterWeight);
        secondary.rgb /= max(secondary.a, 0.001);
        // 两次交点共用同一发射和频移模型, 由沿光路的遮挡决定可见度, 不人为切换上下光弧能量.
        vec3 radiance = vec3(0.0);
        float transmission = 1.0;
        float rimVisibility = 1.0 - smoothstep(0.025, 0.1, abs(inclination));
        if (rimVisibility > 0.0) {
          vec4 rim = foregroundRim(discBasis * localEye, discBasis * ray, observerLapse, footprint);
          rim.a *= rimVisibility;
          accumulate(rim, radiance, transmission);
        }
        accumulate(primary, radiance, transmission);
        accumulate(secondary, radiance, transmission);
        float shadow = (1.0 - smoothstep(shadowRadius - footprint, shadowRadius + footprint, impact)) * step(radialDirection, 0.0);
        // 核心暗部在盘像之后合成, 前景流纹和盘缘仍按透射率遮住核心, 不添加独立实体表面.
        if (shadow > 0.0 && transmission > 0.005) radiance += transmission * shadow * coreRelief(ray, impact, observerLapse);
        float opacity = 1.0 - transmission * (1.0 - shadow);
        // 内侧散射收紧, 外侧逐渐展开; 只从真实盘像取得能量, 显式粗 mip 避免细纹闪烁.
        vec3 glow = vec3(0.0);
        float outerScatter = smoothstep(shadowRadius * 1.5, ${optics.discOuter.toFixed(8)}, impact);
        float spread = max(0.24 + outerScatter * outerScatter * 1.5, footprint * 1.5);
        for (int index = 0; index < 2; index++) {
          float offsetImpact = clamp(impact + (float(index) * 2.0 - 1.0) * spread, minImpact, maxImpact);
          float glowColumn = impactColumn(offsetImpact);
          vec4 nearGlow = discImage(glowColumn, phase, cameraPhase, observer, tangent, angularMomentum, observerLapse, 5.0);
          vec4 farGlow = discImage(glowColumn, phase + 3.14159265, cameraPhase, observer, tangent, angularMomentum, observerLapse, 5.0);
          glow += exposePlasma(nearGlow.rgb) * nearGlow.a + exposePlasma(farGlow.rgb) * farGlow.a * (1.0 - nearGlow.a);
        }
        glow *= mix(0.055, 0.045, outerScatter) * (1.0 - shadow) * (1.0 - smoothstep(maxImpact - 1.0, maxImpact, impact));

        // 使用光学节点附近的虚拟深度, 不把包围球前表面当成实体遮挡邻近星体.
        vec3 hit = localEye + ray * max(0.1, -dot(localEye, ray));
        vec4 clip = projectionMatrix * modelViewMatrix * vec4(hit, 1.0);
        gl_FragDepth = clip.z / clip.w * 0.5 + 0.5;
        if (backgroundEnabled > 0.5) {
          vec3 background = lensBackground(observer, tangent, angles.g - cameraPhase, impact, gl_FragDepth);
          // 背景保持原曝光, 只有盘面压缩高光; 已合成背景的片元输出不透明颜色, 避免重复叠加.
          gl_FragColor = vec4(exposePlasma(radiance) + glow + background * (1.0 - opacity), 1.0);
        } else {
          opacity = max(opacity, max(glow.r, max(glow.g, glow.b)));
          if (opacity < 0.002) discard;
          gl_FragColor = vec4((exposePlasma(radiance) + glow) / max(opacity, 0.001), opacity);
        }
        #include <tonemapping_fragment>
        #include <colorspace_fragment>
      }
    `;
