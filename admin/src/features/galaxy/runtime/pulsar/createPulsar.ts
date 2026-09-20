// 创建程序化三维脉冲星; 所有几何与材质交由场景作用域释放, 自转由外部唯一帧循环驱动.
import * as THREE from "three";
import type { ResourceScope } from "../resourceScope.ts";
import { pulsarConfig } from "./config.ts";
import { surfaceVertex, surfaceFragment, haloFragment, beamVertex, beamFragment } from "./shaders.ts";
import { createMagneticField } from "./magneticField.ts";

// core 提供实体拾取, rotor 接受速度接口; 双极光束与磁场继承同一磁轴姿态.
export function createPulsar(scope: ResourceScope) {
  const config = pulsarConfig;
  const phase = { value: 0 };
  let elapsed = 0;
  const group = new THREE.Group();
  group.name = "Pulsar";
  const inclination = new THREE.Group();
  inclination.rotation.x = config.spinTilt;
  const rotor = new THREE.Group();
  rotor.name = "PulsarRotor";
  group.add(inclination);
  inclination.add(rotor);
  const magneticFrame = new THREE.Group();
  magneticFrame.rotation.z = config.magneticTilt;
  rotor.add(magneticFrame);
  const magneticAxis = new THREE.Vector3(-Math.sin(config.magneticTilt), Math.cos(config.magneticTilt), 0);
  const sphere = scope.own(new THREE.SphereGeometry(config.coreRadius, 64, 40));
  const core = new THREE.Mesh(
    sphere,
    scope.own(
      new THREE.ShaderMaterial({
        uniforms: { magneticAxis: { value: magneticAxis }, phase },
        vertexShader: surfaceVertex,
        fragmentShader: surfaceFragment,
      }),
    ),
  );
  core.name = "PulsarCore";
  rotor.add(core);
  const haloCamera = new THREE.Vector3();
  const halo = new THREE.Mesh(
    scope.own(new THREE.SphereGeometry(config.haloRadius, 64, 40)),
    scope.own(
      new THREE.ShaderMaterial({
        uniforms: {
          magneticAxis: { value: magneticAxis },
          localCamera: { value: haloCamera },
          coreRadius: { value: config.coreRadius },
          haloRadius: { value: config.haloRadius },
        },
        vertexShader: beamVertex,
        fragmentShader: haloFragment,
        transparent: true,
        depthWrite: false,
        side: THREE.BackSide,
        blending: THREE.CustomBlending,
        blendSrc: THREE.OneFactor,
        blendDst: THREE.OneFactor,
        blendSrcAlpha: THREE.ZeroFactor,
        blendDstAlpha: THREE.OneFactor,
      }),
    ),
  );
  halo.name = "PulsarHalo";
  halo.onBeforeRender = (_renderer, _scene, camera) => {
    haloCamera.setFromMatrixPosition(camera.matrixWorld);
    halo.worldToLocal(haloCamera);
  };
  rotor.add(halo);

  const volume = scope.own(new THREE.CylinderGeometry(config.beamRadius, config.beamRadius, config.beamLength, 48, 1, false));
  volume.translate(0, config.beamLength / 2, 0);
  for (const sign of [1, -1]) {
    const localCamera = new THREE.Vector3();
    const material = scope.own(
      new THREE.ShaderMaterial({
        uniforms: {
          localCamera: { value: localCamera },
          beamLength: { value: config.beamLength },
          beamRadius: { value: config.beamRadius },
          coreRadius: { value: config.coreRadius },
          phase,
        },
        vertexShader: beamVertex,
        fragmentShader: beamFragment,
        transparent: true,
        depthWrite: false,
        side: THREE.BackSide,
        // 光积分已预乘透射率, 使用 One/One 加法, 避免再次乘 alpha 导致远景过暗.
        blending: THREE.CustomBlending,
        blendSrc: THREE.OneFactor,
        blendDst: THREE.OneFactor,
        blendSrcAlpha: THREE.ZeroFactor,
        blendDstAlpha: THREE.OneFactor,
      }),
    );
    const beam = new THREE.Mesh(volume, material);
    beam.name = sign > 0 ? "PulsarNorthBeam" : "PulsarSouthBeam";
    beam.rotation.z = sign > 0 ? 0 : Math.PI;
    // 每个光束独占相机 uniform; 世界姿态由渲染器更新后再转换, 不分配逐帧临时对象.
    beam.onBeforeRender = (_renderer, _scene, camera) => {
      localCamera.setFromMatrixPosition(camera.matrixWorld);
      beam.worldToLocal(localCamera);
    };
    magneticFrame.add(beam);
  }

  magneticFrame.add(createMagneticField(scope, phase));
  // 流动相位只在真实帧增量上推进; 所有着色函数以 2π 周期闭合, 包裹时不跳变.
  function update(deltaSeconds: number): void {
    if (!Number.isFinite(deltaSeconds) || deltaSeconds <= 0) return;
    elapsed = (elapsed + (deltaSeconds % config.flowSeconds)) % config.flowSeconds;
    phase.value = (elapsed / config.flowSeconds) * Math.PI * 2;
  }
  return { group, rotor, core, update };
}
