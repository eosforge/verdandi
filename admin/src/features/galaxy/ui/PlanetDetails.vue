<script setup lang="ts">
import { computed, ref, watch } from "vue";
import { NButton } from "naive-ui";
import { planetColors, planetKinds } from "../model/presentation.ts";
import type { Star, Planet, PlanetKind } from "../model/types.ts";
const props = defineProps<{ star: Star; planet: Planet; neighborCount: number; sourceLabel: string; description: string }>();
const emit = defineEmits<{ close: []; focus: []; select: [planetId: string] }>();
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
  <aside class="system-panel" aria-label="星系详情">
    <div class="panel-heading">
      <span class="eyebrow">SYSTEM DETAILS</span><NButton quaternary size="small" aria-label="关闭详情并返回全景" @click="emit('close')">✕</NButton>
    </div>
    <div class="star-identity">
      <span class="identity-orb" :style="{ '--star-color': star.color }"></span>
      <div>
        <h2>{{ star.name }}</h2>
        <span>{{ star.id }}</span>
      </div>
    </div>
    <p class="panel-description">恒星代表 Star, 各类实体沿不同倾角的椭圆轨道公转。查看行星详情时暂停公转。</p>
    <div class="star-facts">
      <div>
        <span>关联行星</span><strong>{{ star.planets.length }}</strong>
      </div>
      <div>
        <span>邻接恒星</span><strong>{{ neighborCount }}</strong>
      </div>
      <div>
        <span>数据来源</span><strong class="demo-value">{{ sourceLabel }}</strong>
      </div>
    </div>

    <div class="planet-detail" aria-live="polite">
      <div class="eyebrow" :style="{ color: planetColors[planet.kind] }">{{ planet.kind }}</div>
      <h3>{{ planet.name }}</h3>
      <dl>
        <dt>实体标识</dt>
        <dd>{{ planet.id }}</dd>
        <dt>关联 Star</dt>
        <dd>{{ star.id }}</dd>
        <dt>实时状态</dt>
        <dd>未接入 · 演示实体</dd>
      </dl>
      <NButton block secondary size="small" @click="emit('focus')">推进至行星</NButton>
    </div>

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
    <p class="demo-note">{{ description }}</p>
  </aside>
</template>
<style scoped>
.eyebrow {
  font-size: 10px;
  letter-spacing: 2px;
  color: #7288a5;
}
.identity-orb {
  flex-shrink: 0;
  border-radius: 50%;
  box-shadow: 0 0 18px color-mix(in srgb, var(--star-color), transparent 45%);
}
.system-panel {
  position: absolute;
  right: 0;
  top: 0;
  bottom: 0;
  z-index: 2;
  display: flex;
  flex-direction: column;
  flex: 0 0 280px;
  width: 280px;
  box-sizing: border-box;
  min-height: 0;
  overflow-y: auto;
  padding: 40px 20px 16px;
  background: #0c1320;
  border-left: 1px solid #ffffff0c;
  scrollbar-width: thin;
  scrollbar-color: #29364c transparent;
}
.panel-heading {
  display: flex;
  align-items: center;
  justify-content: space-between;
}
.panel-heading .eyebrow {
  font-size: 9px;
}
.star-identity {
  display: flex;
  align-items: center;
  gap: 16px;
  margin-top: 18px;
}
.identity-orb {
  width: 32px;
  height: 32px;
  background: radial-gradient(circle at 35% 30%, #fff4, transparent 65%), var(--star-color);
}
.star-identity h2 {
  margin: 0;
  font-weight: 500;
  font-size: 24px;
  line-height: 1.2;
}
.star-identity div > span {
  color: #708099;
  font-size: 11px;
}
.panel-description {
  margin: 20px 0;
  color: #8393ad;
  font-size: 11px;
  line-height: 1.9;
}
.star-facts {
  display: grid;
  grid-template-columns: repeat(3, 1fr);
  padding: 16px 0;
  border-top: 1px solid #ffffff0a;
  border-bottom: 1px solid #ffffff0a;
}
.star-facts > div {
  display: flex;
  flex-direction: column;
  gap: 6px;
}
.star-facts span {
  font-size: 10px;
  color: #71829b;
}
.star-facts strong {
  font-size: 20px;
  font-weight: 400;
}
.star-facts .demo-value {
  font-size: 13px;
  color: #cbb994;
  margin-top: 4px;
}
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
.demo-note {
  margin: 20px 0 0;
  color: #61718b;
  font-size: 10px;
  line-height: 1.8;
}
.planet-detail {
  padding: 16px;
  margin-top: 18px;
  border: 1px solid #a9c7ff1a;
  border-radius: 6px;
  background: #111c2d;
}
.planet-detail .eyebrow {
  font-size: 9px;
  letter-spacing: 1px;
}
.planet-detail h3 {
  margin: 6px 0 16px;
  font-size: 16px;
  font-weight: 500;
}
.planet-detail dl {
  margin: 0 0 14px;
  font-size: 10px;
}
.planet-detail dt {
  color: #70829e;
  margin-top: 10px;
}
.planet-detail dd {
  color: #bcc9dd;
  margin: 4px 0 0;
  overflow-wrap: anywhere;
}
button:focus-visible {
  outline: 2px solid #83d8c8;
  outline-offset: 3px;
}
@media (max-width: 760px) {
  .system-panel {
    width: min(260px, 80%);
  }
}
</style>
