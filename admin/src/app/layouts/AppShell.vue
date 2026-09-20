<!-- 全视口应用容器; 星图工具与详情由功能模块自行管理. -->
<template>
  <main class="shell" aria-label="星空总览"><slot /></main>
</template>

<style scoped>
.shell {
  width: 100%;
  height: 100vh;
  height: 100dvh;
}
</style>
