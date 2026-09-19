// 将已解码的自有 GLB 绑定到光学材质; 模型克隆共享这些场景资源.
import * as THREE from "three";
import { createHorizonMaterial } from "./material.ts";
import { createBendingTexture, createImageBoundsTexture, createObserverTexture } from "./lookupTextures.ts";
import { createPlasmaTexture } from "./plasmaTexture.ts";
import type { ResourceScope } from "../resourceScope.ts";

// 原始 GLB 已登记; 新增纹理和材质交给同一作用域, 失败由场景统一回滚.
export function prepareBlackHoleModel(model: THREE.Group, scope: ResourceScope): void {
  const horizon = model.getObjectByName("Horizon");
  const accretion = model.getObjectByName("Accretion");
  if (!(horizon instanceof THREE.Mesh) || !accretion) throw new Error("Invalid black-hole accretion group");
  const discNormal = new THREE.Vector3(0, 1, 0).applyQuaternion(accretion.quaternion);
  const discBasis = new THREE.Matrix3().setFromMatrix4(new THREE.Matrix4().makeRotationFromQuaternion(accretion.quaternion).invert());
  const bending = scope.own(createBendingTexture());
  const imageBounds = scope.own(createImageBoundsTexture(bending));
  const observerTable = scope.own(createObserverTexture());
  const plasma = scope.own(createPlasmaTexture());
  horizon.material = scope.own(createHorizonMaterial(discNormal, discBasis, bending, imageBounds, observerTable, plasma));
  // 原始盘体保留建模契约, 核心保留射线拾取; 光学体积统一成像, 避免普通几何产生半球和双盘.
  for (const name of ["Core", "Disc", "Glow"]) {
    const mesh = model.getObjectByName(name);
    if (mesh) mesh.visible = false;
  }
}
