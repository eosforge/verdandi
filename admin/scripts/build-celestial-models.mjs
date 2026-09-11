import { mkdir, writeFile } from "node:fs/promises";
import * as THREE from "three";
import { GLTFExporter } from "three/addons/exporters/GLTFExporter.js";
import { blackHoleOptics } from "../src/features/galaxy/runtime/materials/blackHoleOptics.ts";

// 仅适配导出器读取本地 Blob 的接口; 不访问网络或加载外部贴图.
class ExportFileReader {
  // 异步读取内存 Blob, 保留导出器设置 onloadend 的时机.
  readAsArrayBuffer(blob) {
    blob.arrayBuffer().then((result) => {
      this.result = result;
      this.onloadend?.();
    });
  }
}

// 给实体网格烘焙径向温差与角向亮度, 避免把吸积盘画成重复的同心圆线条.
function colorAccretion(geometry) {
  const positions = geometry.getAttribute("position");
  const colors = [];
  const color = new THREE.Color();
  const copper = new THREE.Color("#b92f07");
  const orange = new THREE.Color("#ff852b");
  const hot = new THREE.Color("#fff4da");
  for (let index = 0; index < positions.count; index++) {
    const x = positions.getX(index);
    const z = positions.getZ(index);
    const radius = Math.hypot(x, z);
    const angle = Math.atan2(z, x);
    const heat = Math.exp(-Math.max(0, radius - blackHoleOptics.discInner * 1.36) * 0.16);
    const flow = Math.sin(angle * 3 + radius * 1.1) * 0.06 + Math.sin(angle * 7 - radius * 0.6) * 0.035;
    color
      .copy(copper)
      .lerp(orange, heat)
      .lerp(hot, heat ** 2.2);
    color.multiplyScalar(0.9 + flow);
    const fade =
      THREE.MathUtils.smoothstep(radius, blackHoleOptics.discInner, blackHoleOptics.discInner + 0.6) *
      (1 - THREE.MathUtils.smoothstep(radius, blackHoleOptics.discFadeStart, blackHoleOptics.discOuter));
    colors.push(color.r, color.g, color.b, fade);
  }
  geometry.setAttribute("color", new THREE.Float32BufferAttribute(colors, 4));
  geometry.deleteAttribute("uv");
}

// 构造闭合且有厚度的盘体, 所有表面来自实际三角形, 顶点色只负责材质外观.
function createDisc() {
  const { discInner, discOuter, discHalfHeight } = blackHoleOptics;
  const geometry = new THREE.TorusGeometry((discOuter + discInner) * 0.5, (discOuter - discInner) * 0.5, 40, 256);
  geometry.rotateX(Math.PI / 2);
  geometry.scale(1, discHalfHeight / ((discOuter - discInner) * 0.5), 1);
  colorAccretion(geometry);
  return geometry;
}

// 闭合椭球界定盘面辉光体积, 运行时只在该网格覆盖范围内作固定次数的密度采样.
function createGlowVolume() {
  const geometry = new THREE.SphereGeometry(1, 48, 24);
  geometry.scale(blackHoleOptics.discOuter + 1.75, 2.4, blackHoleOptics.discOuter + 1.75);
  return geometry;
}

// 黑洞由核心、光学球壳、连续盘体和局部辉光体积组成, 不包含独立发光管线或邻接边.
function createBlackHoleModel() {
  const root = new THREE.Group();
  root.name = "BlackHole";
  const core = new THREE.Mesh(new THREE.SphereGeometry(blackHoleOptics.shadowRadius, 128, 96), new THREE.MeshBasicMaterial({ color: 0x000000 }));
  core.name = "Core";
  const horizon = new THREE.Mesh(
    new THREE.SphereGeometry(blackHoleOptics.volumeRadius, 48, 32),
    new THREE.MeshBasicMaterial({ color: 0xf6a54a, transparent: true, opacity: 0.035 }),
  );
  horizon.name = "Horizon";
  const accretion = new THREE.Group();
  accretion.name = "Accretion";
  accretion.rotation.set(-0.4, 0, 0.1);
  const material = new THREE.MeshBasicMaterial({ vertexColors: true, transparent: true, depthWrite: false });
  const disc = new THREE.Mesh(createDisc(), material);
  disc.name = "Disc";
  const glow = new THREE.Mesh(createGlowVolume(), new THREE.MeshBasicMaterial({ color: 0xff681c, transparent: true, opacity: 0.035 }));
  glow.name = "Glow";
  accretion.add(disc, glow);
  root.add(core, horizon, accretion);
  return root;
}

// 以低幅起伏建出实体表面, 不改变平均半径; 几何一次离线生成并复用.
function sculptSurface(radius, widthSegments, heightSegments, amplitude) {
  const geometry = new THREE.SphereGeometry(radius, widthSegments, heightSegments);
  const position = geometry.getAttribute("position");
  for (let index = 0; index < position.count; index++) {
    const x = position.getX(index) / radius;
    const y = position.getY(index) / radius;
    const z = position.getZ(index) / radius;
    const relief = 1 + amplitude * (Math.sin(x * 9 + z * 4) * Math.sin(y * 7 - x * 3) + 0.3 * Math.cos(z * 13 + y * 6));
    position.setXYZ(index, x * radius * relief, y * radius * relief, z * radius * relief);
  }
  geometry.computeVertexNormals();
  const normal = geometry.getAttribute("normal");
  const direction = new THREE.Vector3();
  for (let index = 0; index < normal.count; index++) {
    direction.fromBufferAttribute(normal, index);
    if (direction.lengthSq() < 0.5) direction.fromBufferAttribute(position, index);
    direction.normalize();
    normal.setXYZ(index, direction.x, direction.y, direction.z);
  }
  return geometry;
}

// 恒星具有起伏球面和实体日冕壳, 运行时只替换表面材质和颜色.
function createStarModel() {
  const root = new THREE.Group();
  root.name = "Star";
  const surface = new THREE.Mesh(sculptSurface(4.5, 48, 32, 0.012), new THREE.MeshBasicMaterial({ color: 0xffc986 }));
  surface.name = "Surface";
  const corona = new THREE.Mesh(new THREE.SphereGeometry(6.2, 32, 20), new THREE.MeshBasicMaterial({ color: 0xffc986, transparent: true, opacity: 0.1 }));
  corona.name = "Corona";
  root.add(surface, corona);
  return root;
}

// 行星采用共享的简化岩质网格, 几何细节烘焙到顶点, 保持实例化兼容.
function createPlanetModel() {
  const root = new THREE.Group();
  root.name = "Planet";
  const surface = new THREE.Mesh(sculptSurface(0.78, 20, 14, 0.035), new THREE.MeshStandardMaterial({ color: 0xffffff, roughness: 0.88 }));
  surface.name = "Surface";
  root.add(surface);
  return root;
}

// 将自有网格导出到功能目录, 不引用纹理或远程资产; 返回后释放导出专用资源.
async function exportModel(root, name) {
  root.userData = { author: "Verdandi", source: "admin/scripts/build-celestial-models.mjs" };
  const originalFileReader = globalThis.FileReader;
  globalThis.FileReader = ExportFileReader;
  try {
    const bytes = await new GLTFExporter().parseAsync(root, { binary: true, onlyVisible: true });
    const target = new URL(`../src/features/galaxy/runtime/assets/${name}.glb`, import.meta.url);
    await mkdir(new URL(".", target), { recursive: true });
    await writeFile(target, new Uint8Array(bytes));
    console.log(`Wrote ${name}.glb (${bytes.byteLength} bytes)`);
  } finally {
    if (originalFileReader === undefined) delete globalThis.FileReader;
    else globalThis.FileReader = originalFileReader;
    const resources = new Set();
    root.traverse((object) => {
      if (object.isMesh) {
        resources.add(object.geometry);
        resources.add(object.material);
      }
    });
    for (const resource of resources) resource.dispose();
  }
}

await exportModel(createBlackHoleModel(), "black-hole");
await exportModel(createStarModel(), "star");
await exportModel(createPlanetModel(), "planet");
