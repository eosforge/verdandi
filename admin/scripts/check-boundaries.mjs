import { existsSync, readFileSync, readdirSync } from "node:fs";
import { dirname, relative, resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";
import ts from "typescript";

const root = fileURLToPath(new URL("../", import.meta.url));
const sourceRoot = resolve(root, "src");
const failures = [];
let importsChecked = 0;

// 只遍历项目源码, 不执行模块或访问网络.
function sourceFiles(directory) {
  return readdirSync(directory, { withFileTypes: true }).flatMap((entry) => {
    const path = resolve(directory, entry.name);
    return entry.isDirectory() ? sourceFiles(path) : /\.(ts|vue)$/.test(path) ? [path] : [];
  });
}

// 将平台路径统一为源码相对路径, 供显式分层规则判断.
function sourcePath(path) {
  return relative(sourceRoot, path).split(sep).join("/");
}

// 用 TypeScript 语法树读取静态和动态依赖, Vue 仅取 script 块.
function readImports(path) {
  const text = readFileSync(path, "utf8");
  const script = path.endsWith(".vue") ? [...text.matchAll(/<script\b[^>]*>([\s\S]*?)<\/script>/g)].map((match) => match[1]).join("\n") : text;
  const ast = ts.createSourceFile(path, script, ts.ScriptTarget.Latest, true);
  const imports = [];
  // 记录依赖是否动态加载, 防止 UI 入口意外静态引入 WebGL 运行时.
  function visit(node) {
    if ((ts.isImportDeclaration(node) || ts.isExportDeclaration(node)) && node.moduleSpecifier && ts.isStringLiteral(node.moduleSpecifier)) {
      imports.push({ value: node.moduleSpecifier.text, dynamic: false });
    }
    if (ts.isCallExpression(node) && node.expression.kind === ts.SyntaxKind.ImportKeyword) {
      const argument = node.arguments[0];
      if (argument && ts.isStringLiteral(argument)) imports.push({ value: argument.text, dynamic: true });
      else failures.push(`${sourcePath(path)}: dynamic imports must use a literal path`);
    }
    ts.forEachChild(node, visit);
  }
  visit(ast);
  return imports;
}

// 每层只依赖本层或明确允许的下层; app 是唯一装配入口.
function allowed(from, to, dependency) {
  if (from === "main.ts") return to ? to.startsWith("app/") : dependency.value === "vue";
  if (from.startsWith("app/")) return !to || to.startsWith("app/") || /^features\/[^/]+\/(index\.ts|data\/demo\.ts)$/.test(to);
  const match = /^features\/([^/]+)\/(.+)$/.exec(from);
  if (!match) return false;
  const [, feature, file] = match;
  const prefix = `features/${feature}/`;
  if (to && !to.startsWith(prefix)) return false;
  const destination = to?.slice(prefix.length);
  if (file === "index.ts") return !!destination && /^(ui|model)\//.test(destination);
  if (file.startsWith("model/")) return destination?.startsWith("model/");
  if (file.startsWith("data/")) return !!destination && /^(data|model)\//.test(destination);
  if (file.startsWith("runtime/")) return to ? /^(runtime|model)\//.test(destination) : /^three(?:\/|$)/.test(dependency.value);
  if (file.startsWith("composables/")) {
    return to
      ? /^(model|composables)\//.test(destination) || (destination === "runtime/createGalaxyScene.ts" && dependency.dynamic)
      : dependency.value === "vue";
  }
  if (file.startsWith("ui/")) return to ? /^(ui|model|composables)\//.test(destination) : ["vue", "naive-ui"].includes(dependency.value);
  return false;
}

for (const file of sourceFiles(sourceRoot)) {
  const from = sourcePath(file);
  for (const dependency of readImports(file)) {
    importsChecked++;
    const path = dependency.value.startsWith(".") ? resolve(dirname(file), dependency.value) : null;
    const to = path ? sourcePath(path) : null;
    if (path && !existsSync(path)) failures.push(`${from}: unresolved import ${dependency.value}`);
    if (!allowed(from, to, dependency)) failures.push(`${from}: forbidden dependency ${dependency.value}`);
  }
}

if (failures.length) {
  console.error(failures.join("\n"));
  process.exitCode = 1;
} else console.log(`Architecture boundaries passed (${importsChecked} imports).`);
