<!-- 星图配置入口; 草稿仅在确认且校验通过后发布, 场景生命周期由上层负责. -->
<script setup lang="ts">
import { ref } from "vue";
import { NAlert, NButton, NInput, NInputNumber, NModal, NSelect, NSwitch } from "naive-ui";
import type { GalaxyData } from "../model/types.ts";
import { createDraftStar, draftFromGalaxy, editorLimits, galaxyFromDraft, type DraftStar } from "../model/galaxyEditor.ts";

const props = defineProps<{ data: GalaxyData }>();
const emit = defineEmits<{ confirm: [data: GalaxyData] }>();
const visible = ref(false);
const draft = ref(draftFromGalaxy(props.data));
const error = ref("");
const statusOptions = [
  { label: "正常", value: "available" },
  { label: "黑洞", value: "black-hole" },
];

// 控件值在边界收窄为状态类型; 草稿中的卫星数量保留, 在确认黑洞状态时不生成卫星.
function setStatus(star: DraftStar, value: unknown): void {
  if (value === "available" || value === "black-hole") star.status = value;
}

// 每次打开以当前星图为准; 关闭、Esc 和取消均丢弃未确认内容.
function open(): void {
  draft.value = draftFromGalaxy(props.data);
  error.value = "";
  visible.value = true;
}

// 先构造完整有效快照, 失败时保留草稿及旧星图以便修正.
function confirm(): void {
  try {
    const data = galaxyFromDraft(draft.value);
    emit("confirm", data);
    visible.value = false;
  } catch (cause) {
    error.value = cause instanceof Error ? cause.message : "星图配置无效";
  }
}
</script>

<template>
  <button class="editor-toggle" type="button" aria-label="编辑星图" title="编辑星图" aria-haspopup="dialog" :aria-expanded="visible" @click="open">
    <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M12 4 21 20H3Z" /></svg>
  </button>
  <NModal v-model:show="visible" preset="card" title="编辑星图" class="galaxy-editor" :style="{ width: 'min(620px, calc(100vw - 32px))' }" :bordered="false">
    <form class="editor-form" @submit.prevent="confirm">
      <div class="editor-body">
        <section aria-labelledby="editor-stars">
          <div class="section-heading">
            <h3 id="editor-stars">恒星</h3>
            <NButton size="small" :disabled="draft.stars.length >= editorLimits.stars" @click="draft.stars.push(createDraftStar(draft))">添加恒星</NButton>
          </div>
          <p class="hint">最多 {{ editorLimits.stars }} 颗。正常状态可带 0–{{ editorLimits.satellites }} 颗卫星；黑洞状态不显示卫星或连线，仍参与公转。</p>
          <div v-for="(star, index) in draft.stars" :key="star.id" class="star-row">
            <NInput v-model:value="star.name" :maxlength="64" :input-props="{ 'aria-label': `恒星 ${index + 1} 名称` }" placeholder="恒星名称" />
            <NSelect :value="star.status" :options="statusOptions" :aria-label="`恒星 ${index + 1} 状态`" @update:value="setStatus(star, $event)" />
            <NInputNumber
              v-model:value="star.satellites"
              :disabled="star.status === 'black-hole'"
              :min="0"
              :max="editorLimits.satellites"
              :precision="0"
              :input-props="{ 'aria-label': `恒星 ${index + 1} 卫星数量` }"
              placeholder="卫星数量"
            />
            <NButton quaternary :aria-label="`移除恒星 ${index + 1}`" @click="draft.stars.splice(index, 1)">移除</NButton>
          </div>
          <p v-if="!draft.stars.length" class="empty">尚未添加恒星</p>
        </section>
        <section aria-labelledby="editor-pulsar">
          <div class="section-heading">
            <h3 id="editor-pulsar">脉冲星</h3>
            <NSwitch v-model:value="draft.pulsar.enabled" aria-label="启用脉冲星" />
          </div>
          <p class="hint">最多一颗。启用后，所有恒星（包括黑洞状态）围绕它公转。</p>
          <NInput
            v-if="draft.pulsar.enabled"
            v-model:value="draft.pulsar.name"
            :maxlength="64"
            :input-props="{ 'aria-label': '脉冲星名称' }"
            placeholder="脉冲星名称"
          />
        </section>
      </div>
      <NAlert v-if="error" type="error" role="alert" :show-icon="false">{{ error }}</NAlert>
      <p class="hint">确认后重新生成星图并返回总览；配置仅在当前页面生效。</p>
      <footer>
        <NButton @click="visible = false">取消</NButton>
        <NButton type="primary" attr-type="submit">确认并生成</NButton>
      </footer>
    </form>
  </NModal>
</template>

<style scoped>
.editor-toggle {
  position: absolute;
  right: 18px;
  bottom: 18px;
  z-index: 5;
  display: grid;
  place-items: center;
  width: 44px;
  height: 44px;
  padding: 10px;
  border: 1px solid #91c9e638;
  border-radius: 10px;
  background: #0b1b27d9;
  color: #a9d4e9;
  cursor: pointer;
}
.editor-toggle:hover {
  background: #193247;
  color: #effaff;
}
.editor-toggle:focus-visible {
  outline: 2px solid #8bd4fa;
  outline-offset: 3px;
}
.editor-toggle svg {
  width: 24px;
  height: 24px;
  fill: currentColor;
}
.editor-form {
  display: grid;
  gap: 14px;
}
.editor-body {
  max-height: min(58vh, 580px);
  overflow-y: auto;
  padding-right: 6px;
}
section + section {
  margin-top: 24px;
  border-top: 1px solid #ffffff14;
  padding-top: 18px;
}
.section-heading,
footer {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
}
h3 {
  margin: 0;
  font-size: 15px;
}
.hint,
.empty {
  margin: 8px 0;
  color: #91a4b6;
  font-size: 12px;
}
.star-row {
  display: grid;
  grid-template-columns: minmax(0, 1fr) 88px minmax(100px, 124px) auto;
  align-items: center;
  gap: 8px;
  margin-top: 10px;
}
footer {
  justify-content: flex-end;
}
@media (max-width: 480px) {
  .star-row {
    grid-template-columns: 88px minmax(0, 1fr) auto;
  }
  .star-row > :first-child {
    grid-column: 1 / -1;
  }
}
</style>
