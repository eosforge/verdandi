<script setup lang="ts">
import { h, ref } from "vue";
import { NButton, NLayout, NLayoutContent, NLayoutSider, NMenu, type MenuOption } from "naive-ui";

const collapsed = ref(false);
const menuOptions: MenuOption[] = [
  {
    label: "星空总览",
    key: "home",
    // 内联导航图标沿用现有图形, 无额外图标包依赖.
    icon: () =>
      h(
        "svg",
        {
          viewBox: "0 0 24 24",
          fill: "none",
          stroke: "currentColor",
          "stroke-width": 1.7,
          "stroke-linecap": "round",
          "stroke-linejoin": "round",
          "aria-hidden": true,
        },
        [h("path", { d: "m3 10 9-7 9 7M5 9v11h5v-6h4v6h5V9" })],
      ),
  },
];
</script>

<template>
  <NLayout has-sider class="shell">
    <NLayoutSider
      id="sidebar"
      v-model:collapsed="collapsed"
      bordered
      collapse-mode="width"
      :width="220"
      :collapsed-width="64"
      content-style="display: flex; flex-direction: column; height: 100%"
    >
      <div class="brand" :class="{ 'brand--collapsed': collapsed }" aria-label="Verdandi Supervisor">
        <span class="brand-mark" aria-hidden="true">
          <svg width="22" height="22" viewBox="0 0 24 24" fill="none">
            <path d="m5 5 7 14 7-14" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round" />
          </svg>
        </span>
        <div v-if="!collapsed" class="brand-copy">
          <strong>Verdandi</strong>
          <span>Supervisor</span>
        </div>
      </div>
      <nav class="navigation" aria-label="主导航">
        <NMenu :options="menuOptions" :collapsed="collapsed" :collapsed-width="64" :collapsed-icon-size="20" value="home" />
      </nav>
      <div class="sidebar-footer">
        <NButton
          quaternary
          :aria-label="collapsed ? '展开侧边栏' : '折叠侧边栏'"
          :title="collapsed ? '展开侧边栏' : '折叠侧边栏'"
          :aria-expanded="!collapsed"
          aria-controls="sidebar"
          @click="collapsed = !collapsed"
        >
          <template #icon>
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">
              <path :d="collapsed ? 'm9 6 6 6-6 6' : 'm15 6-6 6 6 6'" />
            </svg>
          </template>
          <span v-if="!collapsed">折叠侧边栏</span>
        </NButton>
      </div>
    </NLayoutSider>
    <NLayoutContent content-style="height: 100%">
      <main aria-label="主页"><slot /></main>
    </NLayoutContent>
  </NLayout>
</template>

<style scoped>
.shell {
  height: 100vh;
  height: 100dvh;
}

.shell :deep(.n-layout-sider) {
  background: var(--color-sidebar);
}

.brand {
  display: flex;
  flex-shrink: 0;
  align-items: center;
  gap: 12px;
  height: 76px;
  padding: 0 16px;
  border-bottom: 1px solid #ffffff0b;
  font-family: system-ui, sans-serif;
}

.brand--collapsed {
  justify-content: center;
  padding: 0;
}

.brand-mark {
  display: grid;
  flex: 0 0 32px;
  height: 32px;
  place-items: center;
  border-radius: 8px;
  background: #18794e;
  color: #fff;
}

.brand-copy {
  display: flex;
  flex-direction: column;
  white-space: nowrap;
}

.brand-copy strong {
  color: #e5ecf8;
  font-size: 18px;
}

.brand-copy span {
  margin-top: 2px;
  color: #8393ad;
  font-size: 12px;
}

.navigation {
  flex: 1;
}

.sidebar-footer {
  display: flex;
  flex-shrink: 0;
  padding: 12px;
  border-top: 1px solid #ffffff0b;
}

main {
  height: 100%;
}
</style>
