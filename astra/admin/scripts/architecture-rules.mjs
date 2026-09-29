// 架构规则只处理路径与依赖元数据, 不读取磁盘或执行项目模块.

// 运行时按资源所有权分组; 底层组件不能反向依赖场景装配或 Vue 适配层.
function allowedRuntime(file, destination, dependency) {
  if (!destination)
    return (
      /^three(?:\/|$)/.test(dependency.value) &&
      !/^runtime\/blackHole\/(shaders\/|config\.ts|flow\.ts|optics\.ts)/.test(file) &&
      !/^runtime\/background\/(shaders|distantStarShaders)\.ts$/.test(file) &&
      file !== "runtime/rendering/layers.ts" &&
      !/^runtime\/pulsar\/(config|shaders)\.ts$/.test(file)
    );
  if (file.startsWith("runtime/blackHole/")) {
    if (/^runtime\/blackHole\/(config|flow)\.ts$/.test(file)) return false;
    if (file === "runtime/blackHole/optics.ts") return destination === "runtime/blackHole/config.ts";
    if (file.startsWith("runtime/blackHole/shaders/")) return /^runtime\/blackHole\/(shaders\/|config\.ts|flow\.ts)/.test(destination);
    return destination.startsWith("runtime/blackHole/") || destination === "runtime/resourceScope.ts";
  }
  if (file.startsWith("runtime/materials/")) return destination.startsWith("runtime/materials/");
  if (file.startsWith("runtime/background/")) {
    if (/\/(shaders|distantStarShaders)\.ts$/.test(file)) return false;
    return /^(runtime\/(background\/|rendering\/|config\.ts|resourceScope\.ts))/.test(destination);
  }
  if (file.startsWith("runtime/rendering/"))
    return file !== "runtime/rendering/layers.ts" && (destination.startsWith("runtime/rendering/") || destination === "runtime/resourceScope.ts");
  if (file.startsWith("runtime/pulsar/")) {
    if (/^runtime\/pulsar\/(config|shaders)\.ts$/.test(file)) return false;
    return destination.startsWith("runtime/pulsar/") || destination === "runtime/resourceScope.ts";
  }
  if (file.startsWith("runtime/objects/"))
    return (
      /^(model\/|runtime\/(objects\/|blackHole\/|pulsar\/|materials\/|config\.ts|resourceScope\.ts))/.test(destination) ||
      (destination === "runtime/celestialAssets.ts" && dependency.typeOnly)
    );
  return /^(runtime|model)\//.test(destination);
}

// 每层仅允许自身和指定下层; Three.js 的唯一动态入口保持在 composable.
export function allowedDependency(from, to, dependency) {
  if (from === "main.ts") return to ? to.startsWith("app/") : dependency.value === "vue";
  if (from.startsWith("app/")) return !to || to.startsWith("app/") || /^features\/[^/]+\/(index\.ts|data\/demo\.ts)$/.test(to);
  const match = /^features\/([^/]+)\/(.+)$/.exec(from);
  if (!match) return false;
  const [, feature, file] = match;
  const prefix = `features/${feature}/`;
  if (to && !to.startsWith(prefix)) return false;
  const destination = to?.slice(prefix.length);
  if (file === "index.ts") return !!destination && /^(ui|model)\//.test(destination);
  if (file.startsWith("model/")) return !!destination && destination.startsWith("model/");
  if (file.startsWith("data/")) return !!destination && /^(data|model)\//.test(destination);
  if (file.startsWith("runtime/")) return allowedRuntime(file, destination, dependency);
  if (file.startsWith("composables/")) {
    return to
      ? /^(model|composables)\//.test(destination) || (destination === "runtime/createGalaxyScene.ts" && dependency.dynamic)
      : dependency.value === "vue";
  }
  if (file.startsWith("ui/")) return to ? /^(ui|model|composables)\//.test(destination) : ["vue", "naive-ui"].includes(dependency.value);
  return false;
}

// 只传入有运行时效果的本地导入边; 类型引用不产生模块初始化环.
export function findImportCycles(graph) {
  const visited = new Set();
  const active = new Set();
  const stack = [];
  const cycles = [];
  function visit(file) {
    if (active.has(file)) {
      cycles.push([...stack.slice(stack.indexOf(file)), file]);
      return;
    }
    if (visited.has(file)) return;
    visited.add(file);
    active.add(file);
    stack.push(file);
    for (const next of graph.get(file) ?? []) visit(next);
    stack.pop();
    active.delete(file);
  }
  for (const file of graph.keys()) visit(file);
  return cycles;
}
