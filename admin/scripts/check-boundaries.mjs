// 静态检查源码导入边界与运行时循环依赖, 不执行应用代码或访问网络.
import { existsSync, readFileSync, readdirSync } from "node:fs";
import { dirname, relative, resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";
import ts from "typescript";
import { allowedDependency, findImportCycles } from "./architecture-rules.mjs";

const root = fileURLToPath(new URL("../", import.meta.url));
const sourceRoot = resolve(root, "src");
const failures = [];
const graph = new Map();
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
      const clause = ts.isImportDeclaration(node) ? node.importClause : undefined;
      const bindings = clause?.namedBindings;
      const allNamedTypes = bindings && ts.isNamedImports(bindings) && bindings.elements.length > 0 && bindings.elements.every((entry) => entry.isTypeOnly);
      const typeOnly = ts.isExportDeclaration(node)
        ? node.isTypeOnly ||
          (node.exportClause &&
            ts.isNamedExports(node.exportClause) &&
            node.exportClause.elements.length > 0 &&
            node.exportClause.elements.every((entry) => entry.isTypeOnly))
        : !!clause && (clause.isTypeOnly || (!clause.name && !!allNamedTypes));
      imports.push({ value: node.moduleSpecifier.text, dynamic: false, typeOnly: !!typeOnly });
    }
    if (ts.isCallExpression(node) && node.expression.kind === ts.SyntaxKind.ImportKeyword) {
      const argument = node.arguments[0];
      if (argument && ts.isStringLiteral(argument)) imports.push({ value: argument.text, dynamic: true, typeOnly: false });
      else failures.push(`${sourcePath(path)}: dynamic imports must use a literal path`);
    }
    ts.forEachChild(node, visit);
  }
  visit(ast);
  return imports;
}

for (const file of sourceFiles(sourceRoot)) {
  const from = sourcePath(file);
  const edges = [];
  graph.set(from, edges);
  for (const dependency of readImports(file)) {
    importsChecked++;
    const path = dependency.value.startsWith(".") ? resolve(dirname(file), dependency.value) : null;
    const to = path ? sourcePath(path) : null;
    if (path && !existsSync(path)) failures.push(`${from}: unresolved import ${dependency.value}`);
    if (to && !dependency.typeOnly) edges.push(to);
    if (!allowedDependency(from, to, dependency)) failures.push(`${from}: forbidden dependency ${dependency.value}`);
  }
}

for (const cycle of findImportCycles(graph)) failures.push(`Runtime import cycle: ${cycle.join(" -> ")}`);

if (failures.length) {
  console.error(failures.join("\n"));
  process.exitCode = 1;
} else console.log(`Architecture boundaries passed (${importsChecked} imports).`);
