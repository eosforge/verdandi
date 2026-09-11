<script setup lang="ts">
import { NConfigProvider, darkTheme, dateZhCN, zhCN } from "naive-ui";
import { GalaxyView } from "../features/galaxy/index.ts";
import { demoGalaxy } from "../features/galaxy/data/demo.ts";
import AppShell from "./layouts/AppShell.vue";
</script>
<template>
  <NConfigProvider :theme="darkTheme" :locale="zhCN" :date-locale="dateZhCN">
    <AppShell><GalaxyView :data="demoGalaxy" /></AppShell>
  </NConfigProvider>
</template>
