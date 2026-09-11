<script setup lang="ts">
import { computed, ref } from "vue";
import { NButton } from "naive-ui";
import { useGalaxyScene } from "../composables/useGalaxyScene.ts";
import type { GalaxyData } from "../model/types.ts";
import PlanetDetails from "./PlanetDetails.vue";
const props = defineProps<{ data: GalaxyData }>();
const canvasHost = ref<HTMLElement>();
const { selectedPeer, selectedPlanet, fps, error, retry, overview, selectPlanet, focusPlanet } = useGalaxyScene(canvasHost, () => props.data);
const availableIds = computed(() => new Set(props.data.peers.filter((peer) => peer.status === "available").map((peer) => peer.id)));
const neighborCount = computed(
  () =>
    props.data.links.filter(
      (link) =>
        availableIds.value.has(link.source) &&
        availableIds.value.has(link.target) &&
        (link.source === selectedPeer.value?.id || link.target === selectedPeer.value?.id),
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
      v-if="selectedPeer?.status === 'available' && selectedPlanet"
      :peer="selectedPeer"
      :planet="selectedPlanet"
      :neighbor-count="neighborCount"
      :source-label="data.sourceLabel"
      :description="data.description"
      @close="overview"
      @select="selectPlanet"
      @focus="focusPlanet"
    />
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
.scene-error strong {
  color: #dfb578;
}
.scene-error p {
  color: #8b9bb5;
}
</style>
