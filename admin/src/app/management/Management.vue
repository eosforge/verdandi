<script setup lang="ts">
import { computed, onBeforeUnmount, ref, shallowRef } from "vue";
import { Api, Failure, decimal, object, observation, samples, scopes, successor } from "./api.ts";
import type { Observation, Sample, Scope, Snapshot } from "./api.ts";

const origin = ref(window.location.origin);
const username = ref("");
const password = ref("");
const logged = ref(false);
const busy = ref(false);
const message = ref("");
const monitoring = ref("");
const directory = shallowRef<Observation | null>(null);
const metrics = shallowRef<readonly Sample[]>([]);
const inventory = shallowRef<readonly Scope[]>([]);
const snapshot = shallowRef<Snapshot | null>(null);
const sector = ref("default"),
  spectrum = ref("main"),
  key = ref(""),
  value = ref(""),
  version = ref("1");
const credential = computed(() => sector.value === "__auth" && spectrum.value === "comet");
const measurements = computed(() => new Map(metrics.value.map((sample) => [sample.id, sample])));
let api: Api | null = null;
let lifetime = new AbortController();
let timer: ReturnType<typeof setTimeout> | undefined;

/** 取消本页全部读取及轮询, 旧响应不能跨会话安装; 已发写入无法据此推断未提交. */
function reset() {
  lifetime.abort();
  lifetime = new AbortController();
  clearTimeout(timer);
  timer = undefined;
  directory.value = null;
  metrics.value = [];
  inventory.value = [];
  snapshot.value = null;
  value.value = "";
  logged.value = false;
}
onBeforeUnmount(() => {
  reset();
  lifetime.abort();
});

/** 只呈现固定错误说明, 不显示任意远端正文或 SECRET; 写入异常保持结果不确定. */
function failure(error: unknown, writing = false) {
  if (error instanceof Failure && error.status === 401) logged.value = false;
  message.value = writing
    ? error instanceof Failure && error.effect === "unapplied"
      ? "本次提交未生效. 请读取最新内容并修正请求."
      : "本次提交结果不确定. 不会自动重试; 请重新读取并确认后再决定."
    : error instanceof Error
      ? error.message
      : "读取失败, 请检查后端状态后重试.";
}

/** 登录后另查 Cookie 会话, 跨站 Cookie 被浏览器阻止时明确失败, 不自动切换令牌方案. */
async function connect(existing = false) {
  if (busy.value) return;
  reset();
  busy.value = true;
  const signal = lifetime.signal;
  try {
    const client = new Api(origin.value);
    if (!existing) await client.json("session", signal, "POST", { username: username.value, password: password.value });
    const session = object(await client.json("session", signal));
    if (typeof session.username !== "string" || typeof session.expires !== "string") throw new Error("管理会话响应无效");
    if (signal.aborted) return;
    api = client;
    logged.value = true;
    message.value = `已登录 ${session.username}, 有效期至 ${session.expires}.`;
    void poll(client, signal);
  } catch (error) {
    if (!signal.aborted) failure(error);
  } finally {
    password.value = "";
    busy.value = false;
  }
}

/** 同一轮最多两个观察请求, 完成五秒后再调度; 慢后端不会叠加无界轮询. */
async function poll(client: Api, signal: AbortSignal) {
  try {
    const [nodes, values] = await Promise.allSettled([client.json("nodes", signal), client.json("metrics", signal)]);
    if (nodes.status === "rejected") throw nodes.reason;
    if (values.status === "rejected") throw values.reason;
    const next = observation(nodes.value),
      measured = samples(values.value);
    if (signal.aborted) return;
    directory.value = next;
    metrics.value = measured;
    monitoring.value = next.stale ? `目录观测陈旧: ${next.observed}` : `目录采样: ${next.observed}; 登记不等于在线.`;
  } catch (error) {
    if (signal.aborted) return;
    if (directory.value) directory.value = { ...directory.value, stale: true };
    metrics.value = metrics.value.map((sample) => ({ ...sample, stale: true }));
    monitoring.value = "观测失败, 下方保留旧结果; 不代表节点已经退出.";
    if (error instanceof Failure && error.status === 401) {
      logged.value = false;
      failure(error);
    }
  }
  if (!signal.aborted && logged.value)
    timer = setTimeout(() => {
      void poll(client, signal);
    }, 5000);
}

/** 注销仅操作管理会话, 不停止或删除任何服务和业务数据. */
async function logout() {
  if (!api || busy.value) return;
  const client = api;
  reset();
  api = null;
  busy.value = true;
  try {
    await client.call("session", lifetime.signal, "DELETE");
    message.value = "已注销管理会话.";
  } catch {
    message.value = "注销响应未确认, 本页已清空. 可恢复会话后再次注销.";
  } finally {
    busy.value = false;
  }
}

/** 分组目录失败保留旧结果, 不清空持久配置. */
async function list() {
  if (!api || !logged.value || busy.value) return;
  busy.value = true;
  const signal = lifetime.signal;
  try {
    const next = scopes(await api.json("almanac", signal));
    if (!signal.aborted) inventory.value = next;
  } catch (error) {
    if (!signal.aborted) failure(error);
  } finally {
    busy.value = false;
  }
}

/** 完整快照结束且仍为同一页面请求才替换旧视图; 版本来自 Polaris, 不从 Star 推断. */
async function load() {
  if (!api || !logged.value || busy.value) return;
  busy.value = true;
  const signal = lifetime.signal,
    selectedSector = sector.value,
    selectedSpectrum = spectrum.value;
  try {
    const next = await api.load(selectedSector, selectedSpectrum, signal);
    if (signal.aborted) return;
    snapshot.value = next;
    version.value = successor(next.version) ?? "";
    message.value = `完整读取 ${selectedSector}/${selectedSpectrum}, 版本 ${next.version}, ${next.records.length} 条记录.`;
    if (!version.value) message.value += " 此分组版本已耗尽, 无法继续提交.";
  } catch (error) {
    if (!signal.aborted) failure(error);
  } finally {
    busy.value = false;
  }
}

/** 选择目录条目不隐式写入或载入内容; 耗尽版本明确禁止生成下一次提交. */
function choose(scope: Scope) {
  sector.value = scope.sector;
  spectrum.value = scope.spectrum;
  snapshot.value = null;
  value.value = "";
  version.value = successor(scope.version) ?? "";
  message.value = version.value ? "已选择分组, 可读取完整内容." : "此分组版本已耗尽, 无法继续提交.";
}

/** 单 Key 手动提交, 空 Base64 表示空值; Delete 必须通过单独按钮明确触发. */
async function commit(erase: boolean) {
  if (!api || !logged.value || busy.value) return;
  busy.value = true;
  const signal = lifetime.signal;
  let sent = false;
  try {
    const expected = decimal(version.value);
    if (expected === "0" || !key.value || !sector.value || !spectrum.value) throw new Error("范围、Key 和正版本不能为空");
    if (!erase && (value.value.length > 1398104 || atob(value.value).length > (credential.value ? 4096 : 1048576))) throw new Error("Base64 内容超过上限");
    const body = credential.value
      ? { key: key.value, version: expected, ...(erase ? { erase: true } : { secret: value.value }) }
      : { sector: sector.value, spectrum: spectrum.value, key: key.value, version: expected, ...(erase ? { erase: true } : { value: value.value }) };
    sent = true;
    const result = object(await api.json(credential.value ? "credentials" : "almanac", signal, "POST", body));
    const position = object(result.position);
    if (result.effect !== "committed" || decimal(position.version) !== expected || position.sector !== sector.value || position.spectrum !== spectrum.value)
      throw new Error("提交确认无效");
    if (signal.aborted) return;
    message.value = `Polaris 已持久提交版本 ${expected}. 此确认不表示全部 Star 已安装; 请重新读取后继续编辑.`;
    snapshot.value = null;
    value.value = "";
  } catch (error) {
    if (!signal.aborted) failure(error, sent);
  } finally {
    if (credential.value) value.value = "";
    busy.value = false;
  }
}
</script>

<template>
  <section class="management" aria-label="Astrolabe 管理">
    <h1>Astrolabe</h1>
    <p>真实管理后端 · 登录、节点观测与 Almanac 编辑. 3D 星图仍为独立演示.</p>
    <form class="connection" @submit.prevent="connect()">
      <label>后端来源<input v-model="origin" type="url" required :disabled="logged || busy" autocomplete="url" /></label>
      <label>账号<input v-model="username" required :disabled="logged || busy" maxlength="64" autocomplete="username" /></label>
      <label
        >密码<input v-model="password" type="password" :required="!logged" :disabled="logged || busy" maxlength="1024" autocomplete="current-password"
      /></label>
      <button v-if="!logged" :disabled="busy">登录</button>
      <button v-if="!logged" type="button" :disabled="busy" @click="connect(true)">恢复已有会话</button>
      <button v-else type="button" :disabled="busy" @click="logout">注销</button>
    </form>
    <p class="message" role="status">{{ message }}</p>
    <template v-if="logged">
      <h2>节点观察</h2>
      <p>{{ monitoring }}</p>
      <div class="table">
        <table>
          <thead>
            <tr>
              <th>角色 / 分组</th>
              <th>端点 / 身份</th>
              <th>业务就绪</th>
              <th>参考同步</th>
              <th>会话 / 恢复字节</th>
              <th>指标采样</th>
            </tr>
          </thead>
          <tbody>
            <tr v-for="node in directory?.nodes" :key="node.id">
              <td>{{ node.role }}<br />{{ node.galaxy }} / {{ node.group }}</td>
              <td>
                {{ node.endpoint }}<br /><small>{{ node.id }} · {{ node.epoch }}</small>
              </td>
              <td>{{ measurements.get(node.id)?.values.astra_ready ?? "未知" }}</td>
              <td>{{ measurements.get(node.id)?.values.astra_clock_synchronized ?? "未知" }}</td>
              <td>{{ measurements.get(node.id)?.values.astra_sessions ?? "—" }} / {{ measurements.get(node.id)?.values.astra_recovery_bytes ?? "—" }}</td>
              <td>{{ measurements.get(node.id)?.observed ?? "未配置或尚未采样" }}<strong v-if="measurements.get(node.id)?.stale"> (陈旧)</strong></td>
            </tr>
          </tbody>
        </table>
      </div>
      <h2>Almanac</h2>
      <button :disabled="busy" @click="list">读取分组目录</button>
      <div class="scopes">
        <button v-for="item in inventory" :key="JSON.stringify([item.sector, item.spectrum])" :disabled="busy" @click="choose(item)">
          {{ item.sector }} / {{ item.spectrum }} · {{ item.version }}
        </button>
      </div>
      <fieldset :disabled="busy">
        <div class="connection">
          <label
            >Sector<input
              v-model="sector"
              maxlength="128"
              @input="
                snapshot = null;
                value = '';
              " /></label
          ><label
            >Spectrum<input
              v-model="spectrum"
              maxlength="128"
              @input="
                snapshot = null;
                value = '';
              " /></label
          ><button @click="load">读取完整内容</button>
        </div>
        <div class="connection">
          <label>{{ credential ? "APIKEY" : "Key" }}<input v-model="key" maxlength="1024" /></label
          ><label>本次提交版本<input v-model="version" inputmode="numeric" maxlength="20" /></label>
        </div>
        <label
          >{{ credential ? "新 APISECRET (Base64, 不回显已有 SECRET)" : "完整 Buffer (Base64, 空字符串为合法空值)"
          }}<textarea v-model="value" :maxlength="credential ? 5464 : 1398104" spellcheck="false" autocomplete="off"></textarea>
        </label>
        <div class="connection">
          <button :disabled="!version" @click="commit(false)">提交 Set</button
          ><button class="danger" :disabled="!version" @click="commit(true)">删除此 Key</button>
        </div>
      </fieldset>
      <p>写入成功表示 Polaris 已持久提交. 结果不确定时不会重试. 凭据管理使用 Sector=__auth, Spectrum=comet.</p>
      <div v-if="snapshot" class="table">
        <p>已读取版本 {{ snapshot.version }}, 共 {{ snapshot.records.length }} 条; 表格最多显示 1000 条.</p>
        <table>
          <thead>
            <tr>
              <th>Key</th>
              <th>Value</th>
              <th>编辑</th>
            </tr>
          </thead>
          <tbody>
            <tr v-for="row in snapshot.records.slice(0, 1000)" :key="row.key">
              <td>{{ row.key }}</td>
              <td>{{ row.value === null ? "SECRET 已隐藏" : row.value.slice(0, 96) + (row.value.length > 96 ? "…" : "") }}</td>
              <td>
                <button
                  :disabled="busy"
                  @click="
                    key = row.key;
                    value = row.value ?? '';
                  "
                >
                  选择
                </button>
              </td>
            </tr>
          </tbody>
        </table>
      </div>
    </template>
  </section>
</template>

<style scoped>
.management {
  height: 100%;
  overflow: auto;
  box-sizing: border-box;
  padding: 64px 28px 40px;
  color: #dbe8fa;
  background: #0a1120;
}
h1 {
  margin: 0;
}
h2 {
  margin-top: 28px;
}
p {
  color: #a4b5cc;
}
.connection {
  display: flex;
  flex-wrap: wrap;
  gap: 12px;
  align-items: end;
  margin: 12px 0;
}
label {
  display: grid;
  gap: 6px;
}
input,
textarea,
button {
  border: 1px solid #3a506b;
  border-radius: 5px;
  padding: 8px 12px;
  color: inherit;
  background: #111f32;
  font: inherit;
}
input {
  min-width: 180px;
}
textarea {
  width: 100%;
  min-height: 110px;
  box-sizing: border-box;
}
button {
  cursor: pointer;
}
button:disabled {
  opacity: 0.5;
  cursor: default;
}
.danger {
  border-color: #d78080;
}
fieldset {
  border: 0;
  padding: 0;
}
.message {
  white-space: pre-wrap;
  color: #b8d5fe;
  min-height: 1.5em;
}
.table {
  overflow: auto;
}
table {
  width: 100%;
  border-collapse: collapse;
  text-align: left;
}
th,
td {
  padding: 10px;
  border-bottom: 1px solid #25364d;
  overflow-wrap: anywhere;
}
.scopes {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
  margin: 12px 0;
  max-height: 160px;
  overflow: auto;
}
small {
  color: #9facc0;
}
strong {
  color: #f6c779;
}
</style>
