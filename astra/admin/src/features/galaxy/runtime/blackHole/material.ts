// 创建共享光学材质和 uniform 初值; 纹理所有权归调用者作用域.
import * as THREE from "three";
import { horizonVertexShader, horizonFragmentShader } from "./shaders/horizon.ts";

// 封闭三维网格内统一计算盘像与捕获阴影; 场景拥有共享纹理, 实例在绘制前绑定自己的动画参数.
export function createHorizonMaterial(
  discNormal: THREE.Vector3,
  discBasis: THREE.Matrix3,
  bending: THREE.Texture,
  imageBounds: THREE.Texture,
  observerTable: THREE.Texture,
  plasmaMap: THREE.Texture,
): THREE.ShaderMaterial {
  return new THREE.ShaderMaterial({
    uniforms: {
      discNormal: { value: discNormal.clone().normalize() },
      discBasis: { value: discBasis.clone() },
      bending: { value: bending },
      imageBounds: { value: imageBounds },
      observerTable: { value: observerTable },
      plasmaMap: { value: plasmaMap },
      flowTime: { value: 0 },
      detailLevel: { value: 1 },
      brightKnots: { value: [new THREE.Vector4(), new THREE.Vector4(), new THREE.Vector4()] },
      localEye: { value: new THREE.Vector3() },
      backgroundEnabled: { value: 0 },
      backgroundColor: { value: null },
      backgroundDepth: { value: null },
      backgroundSize: { value: new THREE.Vector2(1, 1) },
      cameraRange: { value: new THREE.Vector2(0.1, 1600) },
    },
    vertexShader: horizonVertexShader,
    fragmentShader: horizonFragmentShader,
    transparent: true,
    blending: THREE.NormalBlending,
    depthWrite: true,
    side: THREE.DoubleSide,
    forceSinglePass: true,
  });
}
