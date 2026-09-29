<script setup lang="ts">
import { computed, ref } from "vue";
import { NButton } from "naive-ui";
import { useGalaxyScene } from "../composables/useGalaxyScene.ts";
import type { GalaxyData } from "../model/types.ts";
import PlanetDetails from "./PlanetDetails.vue";
import GalaxyEditor from "./GalaxyEditor.vue";
const props = defineProps<{ data: GalaxyData }>();
const emit = defineEmits<{ regenerate: [data: GalaxyData] }>();
const canvasHost = ref<HTMLElement>();
const { selectedStar, selectedPlanet, fps, cameraPosition, error, retry, overview, selectPlanet, focusPlanet } = useGalaxyScene(canvasHost, () => props.data);
const availableIds = computed(() => new Set(props.data.stars.filter((star) => star.status === "available").map((star) => star.id)));
const neighborCount = computed(
  () =>
    props.data.links.filter(
      (link) =>
        availableIds.value.has(link.source) &&
        availableIds.value.has(link.target) &&
        (link.source === selectedStar.value?.id || link.target === selectedStar.value?.id),
    ).length,
);
</script>
<template>
  <section class="galaxy" aria-label="集群星图">
    <div class="star-stage">
      <div ref="canvasHost" class="star-canvas"></div>
      <div v-if="error" class="scene-error" role="alert">
        <strong>3D 场景不可用</strong>
        <p>{{ error }}</p>
        <NButton secondary size="small" @click="retry">重试</NButton>
      </div>
    </div>
    <output v-if="!error" class="fps-counter" aria-label="当前 FPS">{{ fps ?? "—" }}</output>
    <PlanetDetails
      v-if="selectedStar?.status === 'available' && selectedPlanet"
      :star="selectedStar"
      :planet="selectedPlanet"
      :neighbor-count="neighborCount"
      :source-label="data.sourceLabel"
      :description="data.description"
      @close="overview"
      @select="selectPlanet"
      @focus="focusPlanet"
    />
    <output v-if="!error && cameraPosition" class="camera-coordinates" aria-label="相机世界坐标" aria-live="off" title="相机世界坐标，可选中复制">
      <span>X {{ cameraPosition[0].toFixed(2) }}</span>
      <span>Y {{ cameraPosition[1].toFixed(2) }}</span>
      <span>Z {{ cameraPosition[2].toFixed(2) }}</span>
    </output>
    <GalaxyEditor :data="data" @confirm="emit('regenerate', $event)" />
  </section>
</template>
<style scoped>
.galaxy {
  position: relative;
  height: 100%;
  min-height: 480px;
  overflow: hidden;
  background: var(--color-space);
  color: #e5ecf8;
  font-family:
    system-ui,
    -apple-system,
    "Microsoft YaHei",
    sans-serif;
}
.star-stage,
.star-canvas {
  position: absolute;
  inset: 0;
}
.star-canvas :deep(canvas) {
  display: block;
  touch-action: none;
}
.fps-counter {
  position: absolute;
  top: 16px;
  right: 20px;
  z-index: 3;
  pointer-events: none;
  color: #92a1b6;
  font:
    12px ui-monospace,
    monospace;
  font-variant-numeric: tabular-nums;
}
.scene-error {
  position: absolute;
  inset: 25% 12% auto;
  border: 1px solid #cb9c4d55;
  border-radius: 8px;
  padding: 24px;
  background: #121925;
  text-align: center;
  font-size: 12px;
}
.camera-coordinates {
  position: absolute;
  right: 74px;
  bottom: 18px;
  z-index: 5;
  display: flex;
  flex-wrap: wrap;
  justify-content: flex-end;
  align-content: center;
  gap: 4px 12px;
  max-width: calc(100% - 90px);
  min-height: 44px;
  color: #a2b7c7;
  font:
    12px ui-monospace,
    monospace;
  font-variant-numeric: tabular-nums;
  user-select: text;
}
.camera-coordinates span {
  white-space: nowrap;
}
.scene-error strong {
  color: #dfb578;
}
.scene-error p {
  color: #8b9bb5;
}
</style>
