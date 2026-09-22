<script setup lang="ts">
import { shallowRef } from "vue";
import { NConfigProvider, darkTheme, dateZhCN, zhCN } from "naive-ui";
import { GalaxyView } from "../features/galaxy/index.ts";
import { demoGalaxy } from "../features/galaxy/data/demo.ts";
import AppShell from "./layouts/AppShell.vue";
import Management from "./management/Management.vue";
const galaxy = shallowRef(demoGalaxy);
const mode = shallowRef<"demo" | "management">("demo");
</script>
<template>
  <NConfigProvider :theme="darkTheme" :locale="zhCN" :date-locale="dateZhCN">
    <AppShell>
      <nav class="mode" aria-label="数据模式">
        <button :aria-pressed="mode === 'demo'" @click="mode = 'demo'">演示星图</button
        ><button :aria-pressed="mode === 'management'" @click="mode = 'management'">真实管理后端</button>
      </nav>
      <GalaxyView v-if="mode === 'demo'" :data="galaxy" @regenerate="galaxy = $event" />
      <Management v-else />
    </AppShell>
  </NConfigProvider>
</template>
<style scoped>
.mode {
  position: absolute;
  top: 12px;
  left: 50%;
  transform: translateX(-50%);
  z-index: 20;
  display: flex;
  gap: 8px;
}
.mode button {
  color: #d8e6fa;
  background: #10203bee;
  border: 1px solid #3e5678;
  border-radius: 6px;
  padding: 8px 12px;
  cursor: pointer;
  white-space: nowrap;
}
.mode button[aria-pressed="true"] {
  border-color: #9fc7ff;
}
</style>
