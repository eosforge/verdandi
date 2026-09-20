<script setup lang="ts">
// 详情面板负责星系与当前行星信息; 列表筛选由 PlanetList 独立维护.
import { NButton } from "naive-ui";
import { planetColors } from "../model/presentation.ts";
import type { Star, Planet } from "../model/types.ts";
import PlanetList from "./PlanetList.vue";

defineProps<{ star: Star; planet: Planet; neighborCount: number; sourceLabel: string; description: string }>();
const emit = defineEmits<{ close: []; focus: []; select: [planetId: string] }>();
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
    <p class="panel-description">恒星代表 Star, 各类实体沿接近共同轨道面的椭圆轨道公转。查看行星详情时暂停公转。</p>
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

    <PlanetList :star="star" :planet="planet" @select="emit('select', $event)" />
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
