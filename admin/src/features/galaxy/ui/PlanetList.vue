<script setup lang="ts">
// 实体分类和选择列表; 只发布稳定 ID, 不依赖场景控制器或 Three.js.
import { computed, ref, watch } from "vue";
import { planetColors, planetKinds } from "../model/presentation.ts";
import type { Star, Planet, PlanetKind } from "../model/types.ts";
const props = defineProps<{ star: Star; planet: Planet }>();
const emit = defineEmits<{ select: [planetId: string] }>();
const activeKind = ref<PlanetKind>(props.planet.kind);
const visiblePlanets = computed(() => props.star.planets.filter((planet) => planet.kind === activeKind.value));
watch(
  () => props.planet,
  (planet) => {
    activeKind.value = planet.kind;
  },
);
</script>
<template>
  <div class="entity-heading">
    <h3>星系实体</h3>
    <span>{{ star.planets.length }} TOTAL</span>
  </div>
  <div class="kind-tabs" role="group" aria-label="实体类型">
    <button v-for="kind in planetKinds" :key="kind" :class="{ active: activeKind === kind }" :aria-pressed="activeKind === kind" @click="activeKind = kind">
      {{ kind }}
    </button>
  </div>
  <div class="planet-list">
    <button
      v-for="planet in visiblePlanets"
      :key="planet.id"
      :class="{ selected: props.planet.id === planet.id }"
      :aria-pressed="props.planet.id === planet.id"
      @click="emit('select', planet.id)"
    >
      <span class="planet-list-dot" :style="{ background: planetColors[planet.kind] }"></span><span>{{ planet.name }}</span
      ><span class="list-arrow" aria-hidden="true">↗</span>
    </button>
  </div>
</template>
<style scoped>
.entity-heading {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-top: 24px;
}
.entity-heading h3 {
  margin: 0;
  font-size: 12px;
  font-weight: 500;
}
.entity-heading > span {
  font-size: 9px;
  letter-spacing: 1px;
  color: #73839c;
}
.kind-tabs {
  display: flex;
  gap: 2px;
  margin: 14px 0 9px;
  padding: 3px;
  background: #060c17;
  border-radius: 5px;
}
.kind-tabs button {
  flex: 1;
  border: 0;
  padding: 7px 3px;
  background: transparent;
  border-radius: 3px;
  font-size: 9px;
  color: #7b8ba4;
  cursor: pointer;
}
.kind-tabs button.active {
  background: #253148;
  color: #e0e9f7;
}
.planet-list {
  min-height: 100px;
  flex: 1 0 160px;
  max-height: 340px;
  overflow-y: auto;
  scrollbar-width: thin;
  scrollbar-color: #29364c transparent;
}
.planet-list button {
  display: flex;
  align-items: center;
  width: 100%;
  padding: 10px 9px;
  gap: 10px;
  border: 1px solid transparent;
  border-radius: 4px;
  color: #b4c1d5;
  background: transparent;
  cursor: pointer;
  text-align: left;
  font-size: 11px;
}
.planet-list button:hover,
.planet-list button.selected {
  background: #192337;
  border-color: #ffffff0a;
  color: #fff;
}
.planet-list-dot {
  width: 5px;
  height: 5px;
  border-radius: 50%;
}
.list-arrow {
  margin-left: auto;
  color: #60728f;
}
button:focus-visible {
  outline: 2px solid #83d8c8;
  outline-offset: 3px;
}
</style>
