import { readFile } from "node:fs/promises";
import { parseCelestialModel } from "../../src/features/galaxy/runtime/celestialAssets.ts";

export const modelFiles = { blackHole: "black-hole", star: "star", planet: "planet" };

export async function readModelBytes(name) {
  const file = await readFile(new URL(`../../src/features/galaxy/runtime/assets/${name}.glb`, import.meta.url));
  return file.buffer.slice(file.byteOffset, file.byteOffset + file.byteLength);
}

export async function loadTestModels(scope) {
  const models = {};
  for (const [kind, name] of Object.entries(modelFiles)) models[kind] = await parseCelestialModel(await readModelBytes(name), kind, scope);
  return models;
}
