import * as THREE from "three";
import { blackHoleOptics } from "./materials/blackHoleOptics.ts";

// 克隆已校验的 GLB 层级, 共享场景拥有的 GPU 资源; 每个节点独占时钟与姿态.
export function createBlackHole(model: THREE.Group) {
  const group = model.clone(true);
  const core = group.getObjectByName("Core");
  const accretion = group.getObjectByName("Accretion");
  const horizon = group.getObjectByName("Horizon");
  if (!(core instanceof THREE.Mesh) || !(horizon instanceof THREE.Mesh) || !(horizon.material instanceof THREE.ShaderMaterial) || !accretion)
    throw new Error("Invalid black-hole model instance");
  const material = horizon.material;
  const flowTime = material.uniforms.flowTime;
  const detailLevel = material.uniforms.detailLevel;
  const brightKnots = material.uniforms.brightKnots;
  const localEye = material.uniforms.localEye;
  if (!flowTime || !detailLevel || !brightKnots || !localEye) throw new Error("Invalid black-hole flow uniforms");
  const position = new THREE.Vector3();
  const cameraPosition = new THREE.Vector3();
  const localCameraPosition = new THREE.Vector3();
  const inverseWorld = new THREE.Matrix4();
  const scale = new THREE.Vector3();
  const viewport = new THREE.Vector2();
  const knots = Array.from({ length: 3 }, () => new THREE.Vector4());
  let elapsedSeconds = 0;

  // 时钟归实例所有, 帧调度器负责暂停和长帧限制; 无效时间不污染后续帧.
  function update(deltaSeconds: number): void {
    if (Number.isFinite(deltaSeconds) && deltaSeconds > 0) elapsedSeconds += deltaSeconds;
  }

  // 共享材质在实际绘制前绑定该实例的时钟和亮结; 不克隆贴图, 不为离屏节点计算亮结.
  horizon.onBeforeRender = (renderer, _scene, camera) => {
    position.setFromMatrixPosition(horizon.matrixWorld);
    cameraPosition.setFromMatrixPosition(camera.matrixWorld);
    scale.setFromMatrixScale(horizon.matrixWorld);
    renderer.getDrawingBufferSize(viewport);
    const radius = blackHoleOptics.shadowRadius * Math.max(scale.x, scale.y, scale.z);
    const pixels = ((Math.abs(camera.projectionMatrix.elements[5] ?? 1) * radius * viewport.y) / Math.max(0.1, position.distanceTo(cameraPosition))) * 0.5;
    const detail = THREE.MathUtils.smoothstep(pixels, 14, 65);
    // 局部观察位置每次绘制只求一次, 避免逐顶点求逆和常量插值造成的盘面临界噪声.
    inverseWorld.copy(horizon.matrixWorld).invert();
    localCameraPosition.copy(cameraPosition).applyMatrix4(inverseWorld);
    localEye.value = localCameraPosition;
    // 整个流场时间提高三倍, 同步加速平流与亮结, 保持每个周期内的最大剪切量一致.
    const flowSeconds = elapsedSeconds * 3;
    flowTime.value = flowSeconds % 48;
    detailLevel.value = detail;
    // 远景保留较宽的亮结作为运动线索, 只有小于数个像素时才淡出, 避免变成闪烁亮点.
    const motionVisibility = THREE.MathUtils.smoothstep(pixels, 2, 8);
    knots.forEach((knot, index) => {
      const duration = 19 + index * 5;
      const life = (flowSeconds + index * 7) / duration;
      const generation = Math.floor(life);
      const age = life - generation;
      const seed = Math.sin(generation * 127.1 + index * 311.7 + 17) * 43758.5453;
      const random = seed - Math.floor(seed);
      const radius = blackHoleOptics.discInner * 1.08 + random * (blackHoleOptics.discOuter - blackHoleOptics.discInner) * 0.45;
      const speed = 0.036 + 0.21 * (4.5 / radius) ** 1.5;
      const angle = (random * Math.PI * 2 + age * duration * speed) % (Math.PI * 2);
      const width = THREE.MathUtils.lerp(0.4 + age * 0.35, 0.09 + age * 0.2, detail);
      knot.set(angle, radius, Math.sin(Math.PI * age) ** 2 * (0.6 + detail * 0.4) * motionVisibility, width);
    });
    brightKnots.value = knots;
    material.uniformsNeedUpdate = true;
  };

  return { group, core, update };
}
