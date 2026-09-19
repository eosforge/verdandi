// 同源模型加载、结构校验和资源登记; 具体星体的光学装配委托所属模块.
import * as THREE from "three";
import { GLTFLoader } from "three/addons/loaders/GLTFLoader.js";
import { prepareBlackHoleModel } from "./blackHole/prepareModel.ts";
import type { ResourceScope } from "./resourceScope.ts";
import type { GalaxyData } from "../model/types.ts";

export type CelestialModelKind = "blackHole" | "star" | "planet";
export type CelestialModels = Partial<Record<CelestialModelKind, THREE.Group>>;

// 解码自有且无外部引用的 GLB, 共享几何与材质归调用者作用域; 失败上抛由场景统一回滚.
export async function parseCelestialModel(bytes: ArrayBuffer, kind: CelestialModelKind, scope: ResourceScope): Promise<THREE.Group> {
  const { scene } = await new GLTFLoader().parseAsync(bytes, "");
  const resources = new Set<THREE.BufferGeometry | THREE.Material>();
  scene.traverse((object) => {
    if (!(object instanceof THREE.Mesh)) return;
    resources.add(object.geometry);
    for (const material of Array.isArray(object.material) ? object.material : [object.material]) {
      resources.add(material);
      if (material.transparent) material.depthWrite = false;
    }
  });
  for (const resource of resources) scope.own(resource);
  const required = kind === "blackHole" ? ["Core", "Horizon", "Disc", "Glow"] : kind === "star" ? ["Surface", "Corona"] : ["Surface"];
  for (const name of required) {
    if (!(scene.getObjectByName(name) instanceof THREE.Mesh)) throw new Error(`Invalid ${kind} model: ${name}`);
  }
  if (kind === "blackHole") prepareBlackHoleModel(scene, scope);
  return scene;
}

// 只读取随应用发布的模型文件; 取消或 HTTP 失败时拒绝, 不创建 WebGL 画布或下载第三方资产.
async function loadModel(url: URL, kind: CelestialModelKind, scope: ResourceScope, signal?: AbortSignal): Promise<THREE.Group> {
  const response = await fetch(url, signal ? { signal } : {});
  if (!response.ok) throw new Error(`Cannot load ${kind} model: ${response.status}`);
  const bytes = await response.arrayBuffer();
  signal?.throwIfAborted();
  const model = await parseCelestialModel(bytes, kind, scope);
  signal?.throwIfAborted();
  return model;
}

// 按快照实际需要加载同源 GLB, 每类仅一份; 等待全部加载收尾后才返回或抛错, 便于统一释放.
export async function loadCelestialModels(data: GalaxyData, scope: ResourceScope, signal?: AbortSignal): Promise<CelestialModels> {
  const models: CelestialModels = {};
  const requests: Promise<void>[] = [];
  if (data.stars.some((star) => star.status === "unavailable"))
    requests.push(
      loadModel(new URL("./assets/black-hole.glb", import.meta.url), "blackHole", scope, signal).then((model) => {
        models.blackHole = model;
      }),
    );
  if (data.stars.some((star) => star.status === "available"))
    requests.push(
      loadModel(new URL("./assets/star.glb", import.meta.url), "star", scope, signal).then((model) => {
        models.star = model;
      }),
    );
  if (data.stars.some((star) => star.status === "available" && star.planets.length))
    requests.push(
      loadModel(new URL("./assets/planet.glb", import.meta.url), "planet", scope, signal).then((model) => {
        models.planet = model;
      }),
    );
  const results = await Promise.allSettled(requests);
  for (const result of results) if (result.status === "rejected") throw result.reason;
  signal?.throwIfAborted();
  return models;
}
