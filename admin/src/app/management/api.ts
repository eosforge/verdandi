/** 管理适配层仅持有后端地址, Cookie 由浏览器管理; 不保存密码、节点凭据或会话 token. */
export class Api {
  readonly origin: string;
  private readonly request: typeof fetch;
  constructor(origin: string, request: typeof fetch = fetch) {
    const url = new URL(origin);
    if (!["https:", "http:"].includes(url.protocol) || url.username || url.password || url.pathname !== "/" || url.search || url.hash) {
      throw new Error("请输入管理后端的完整 HTTP(S) 来源地址");
    }
    this.origin = url.origin;
    // 原生 fetch 不能以 Api 实例作为接收者调用; 构造时绑定全局对象, 每个请求不再分配包装函数.
    this.request = request.bind(globalThis);
  }

  /** 每个请求拥有确定期限; 写入只发一次, 网络异常不会自动重试. */
  async call(path: string, signal: AbortSignal, method = "GET", body?: unknown): Promise<Response> {
    const response = await this.request(`${this.origin}/api/${path}`, {
      method,
      credentials: "include",
      redirect: "error",
      cache: "no-store",
      signal: AbortSignal.any([signal, AbortSignal.timeout(35000)]),
      headers: body === undefined ? { "X-Astra-Request": "1" } : { "X-Astra-Request": "1", "Content-Type": "application/json" },
      ...(body === undefined ? {} : { body: JSON.stringify(body) }),
    });
    if (!response.ok) {
      let effect: "unapplied" | "unknown" = "unknown";
      try {
        if (object(decode(await text(response, 8192))).effect === "unapplied") effect = "unapplied";
      } catch {
        // 截断、超限或非 JSON 错误正文不覆盖 HTTP 身份, 也不能证明写入未发生.
      }
      throw new Failure(
        response.status,
        effect,
        response.status === 401 ? "管理会话已失效, 请重新登录" : `管理请求未成功 (HTTP ${response.status}), 请重新读取后确认结果`,
      );
    }
    return response;
  }

  /** 小型 JSON 接口限制 2 MiB; 大型 Almanac 走明确完整标记的 NDJSON. */
  async json(path: string, signal: AbortSignal, method = "GET", body?: unknown): Promise<unknown> {
    return decode(await text(await this.call(path, signal, method, body), 2 * 1024 * 1024));
  }

  /** 完整接收成功才返回, 部分内容不进入已安装界面, 内部凭据保持脱敏. */
  async load(sector: string, spectrum: string, signal: AbortSignal): Promise<Snapshot> {
    const query = new URLSearchParams({ sector, spectrum });
    const response = await this.call(`almanac?${query}`, signal);
    if (!response.body) throw new Error("快照响应缺少正文");
    const reader = response.body.getReader();
    const decoder = new TextDecoder("utf-8", { fatal: true });
    let pending = "",
      bytes = 0,
      version: string | null = null;
    const records: Row[] = [];
    const keys = new Set<string>();
    const line = (encoded: string) => {
      if (!encoded || version !== null) throw new Error("快照出现空行或完成标记后的数据");
      const value = object(decode(encoded));
      if (value.complete === true) {
        const position = object(value.position);
        if (position.sector !== sector || position.spectrum !== spectrum) throw new Error("快照范围不匹配");
        version = decimal(position.version);
      } else {
        if (
          typeof value.key !== "string" ||
          value.key.length === 0 ||
          keys.has(value.key) ||
          records.length >= 65536 ||
          (value.redacted !== true && typeof value.value !== "string")
        )
          throw new Error("快照记录无效或重复");
        keys.add(value.key);
        records.push({ key: value.key, value: value.redacted === true ? null : String(value.value) });
      }
    };
    try {
      for (;;) {
        const part = await reader.read();
        if (part.done) break;
        bytes += part.value.byteLength;
        if (bytes > 64 * 1024 * 1024) throw new Error("快照超过本地 64 MiB 预算");
        pending += decoder.decode(part.value, { stream: true });
        let end = pending.indexOf("\n");
        while (end >= 0) {
          if (end > 2 * 1024 * 1024) throw new Error("快照单行过大");
          line(pending.slice(0, end));
          pending = pending.slice(end + 1);
          end = pending.indexOf("\n");
        }
        if (pending.length > 2 * 1024 * 1024) throw new Error("快照单行过大");
      }
      pending += decoder.decode();
      if (pending !== "" || version === null) throw new Error("快照未完整结束, 已丢弃本次读取");
      return { version, records };
    } finally {
      await reader.cancel().catch(() => undefined);
      reader.releaseLock();
    }
  }
}

/** 已确认的 HTTP 拒绝可区分无副作用; 断网、超时等普通 Error 一律不能推断未提交. */
export class Failure extends Error {
  readonly status: number;
  readonly effect: "unapplied" | "unknown";
  constructor(status: number, effect: "unapplied" | "unknown", message: string) {
    super(message);
    this.status = status;
    this.effect = effect;
  }
}

export interface Row {
  readonly key: string;
  readonly value: string | null;
}
export interface Snapshot {
  readonly version: string;
  readonly records: readonly Row[];
}
export interface Node {
  readonly id: string;
  readonly role: string;
  readonly galaxy: string;
  readonly group: string;
  readonly endpoint: string;
  readonly epoch: string;
}
export interface Observation {
  readonly nodes: readonly Node[];
  readonly observed: string;
  readonly stale: boolean;
}
export interface Sample {
  readonly id: string;
  readonly observed: string;
  readonly stale: boolean;
  readonly values: Readonly<Record<string, string>>;
}
export interface Scope {
  readonly sector: string;
  readonly spectrum: string;
  readonly version: string;
}

/** 网络输入始终从 unknown 开始, 不用类型断言绕过运行时边界. */
export function object(value: unknown): Record<string, unknown> {
  if (value === null || typeof value !== "object" || Array.isArray(value)) throw new Error("管理响应结构无效");
  return value as Record<string, unknown>;
}

/** 版本是十进制 uint64 字符串, 不经 Number 转换; 零用于合法空基线. */
export function decimal(value: unknown): string {
  if (typeof value !== "string" || !/^(0|[1-9][0-9]{0,19})$/.test(value) || BigInt(value) > 18446744073709551615n) throw new Error("管理版本无效");
  return value;
}

/** 版本耗尽后没有合法的下一次提交; 界面留空, 不回绕也不生成越界请求. */
export function successor(value: string): string | null {
  const current = BigInt(decimal(value));
  return current === 18446744073709551615n ? null : String(current + 1n);
}

/** JSON 解析错误可能带原始内容片段, 转为固定说明防止向界面回显秘密. */
function decode(value: string): unknown {
  try {
    return JSON.parse(value) as unknown;
  } catch {
    throw new Error("管理响应不是有效 JSON");
  }
}

/** 显式解析脱敏目录; 登记存在不等于在线, 不生成虚构连线或黑洞状态. */
export function observation(input: unknown): Observation {
  const value = object(input);
  if (!Array.isArray(value.nodes) || value.nodes.length > 16384 || typeof value.observed !== "string" || typeof value.stale !== "boolean")
    throw new Error("节点观察无效");
  const ids = new Set<string>();
  const nodes = value.nodes.map((raw: unknown): Node => {
    const node = object(raw);
    const fields = [node.id, node.role, node.galaxy, node.group, node.endpoint];
    if (fields.some((field) => typeof field !== "string" || field.length === 0 || field.length > 256)) throw new Error("节点字段无效");
    const id = String(node.id);
    if (ids.has(id)) throw new Error("节点身份重复");
    ids.add(id);
    return { id, role: String(node.role), galaxy: String(node.galaxy), group: String(node.group), endpoint: String(node.endpoint), epoch: decimal(node.epoch) };
  });
  return { nodes, observed: value.observed, stale: value.stale };
}

/** 有界指标投影, 陈旧时仍展示其明确观测时间, 不冒充实时成功. */
export function samples(input: unknown): readonly Sample[] {
  const value = object(input);
  if (!Array.isArray(value.samples) || value.samples.length > 64) throw new Error("指标观察无效");
  return value.samples.map((raw: unknown): Sample => {
    const sample = object(raw);
    if (typeof sample.id !== "string" || typeof sample.observed !== "string" || typeof sample.stale !== "boolean") throw new Error("指标字段无效");
    const values: Record<string, string> = {};
    const entries = sample.values === null ? [] : Object.entries(object(sample.values));
    if (entries.length > 32) throw new Error("指标项过多");
    for (const [key, number] of entries) values[key] = decimal(number);
    return { id: sample.id, observed: sample.observed, stale: sample.stale, values };
  });
}

/** 读取完整分组目录, 不把失败或截断视为空集合. */
export function scopes(input: unknown): readonly Scope[] {
  const value = object(input);
  if (!Array.isArray(value.positions) || value.positions.length > 16384 || value.complete !== true) throw new Error("分组列表无效");
  return value.positions.map((raw: unknown): Scope => {
    const position = object(raw);
    if (typeof position.sector !== "string" || typeof position.spectrum !== "string") throw new Error("分组字段无效");
    return { sector: position.sector, spectrum: position.spectrum, version: decimal(position.version) };
  });
}

/** 所有错误正文也有预算, 不先 response.text() 再检查已分配的大正文. */
async function text(response: Response, maximum: number): Promise<string> {
  if (!response.body) throw new Error("管理响应缺少正文");
  const reader = response.body.getReader(),
    decoder = new TextDecoder("utf-8", { fatal: true });
  let bytes = 0,
    result = "";
  try {
    for (;;) {
      const part = await reader.read();
      if (part.done) break;
      bytes += part.value.byteLength;
      if (bytes > maximum) throw new Error("管理响应超过本地预算");
      result += decoder.decode(part.value, { stream: true });
    }
    return result + decoder.decode();
  } finally {
    await reader.cancel().catch(() => undefined);
    reader.releaseLock();
  }
}
