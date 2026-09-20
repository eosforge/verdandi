<script setup lang="ts">
import { shallowRef } from "vue";
import { NConfigProvider, darkTheme, dateZhCN, zhCN } from "naive-ui";
import { GalaxyView } from "../features/galaxy/index.ts";
import { demoGalaxy } from "../features/galaxy/data/demo.ts";
import AppShell from "./layouts/AppShell.vue";
const galaxy = shallowRef(demoGalaxy);
</script>
<template>
  <NConfigProvider :theme="darkTheme" :locale="zhCN" :date-locale="dateZhCN">
    <AppShell><GalaxyView :data="galaxy" @regenerate="galaxy = $event" /></AppShell>
  </NConfigProvider>
</template>
