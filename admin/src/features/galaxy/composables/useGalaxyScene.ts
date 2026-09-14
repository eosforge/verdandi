import { computed, ref, shallowRef, watch, type Ref } from "vue";
import type { GalaxyController, GalaxyData, GalaxySelection } from "../model/types.ts";

// Vue 生命周期适配器; 快照替换、重试和卸载都先释放旧场景, 过期异步加载不会挂载.
export function useGalaxyScene(host: Ref<HTMLElement | undefined>, getData: () => GalaxyData) {
  const selection = shallowRef<GalaxySelection>(null);
  const fps = ref<number | null>(null);
  const error = ref("");
  const attempt = ref(0);
  let controller: GalaxyController | undefined;

  watch(
    [host, getData, attempt],
    async ([element, data], _previous, onCleanup) => {
      let cancelled = false;
      let instance: GalaxyController | undefined;
      const loading = new AbortController();
      onCleanup(() => {
        cancelled = true;
        loading.abort();
        instance?.dispose();
        if (controller === instance) controller = undefined;
      });
      selection.value = null;
      fps.value = null;
      error.value = "";
      if (!element) return;
      try {
        const { createGalaxyScene } = await import("../runtime/createGalaxyScene.ts");
        if (cancelled) return;
        instance = await createGalaxyScene(
          element,
          data,
          {
            select(value) {
              if (!cancelled) selection.value = value;
            },
            fps(value) {
              if (!cancelled) fps.value = value;
            },
            error(message) {
              if (!cancelled) {
                error.value = message;
                selection.value = null;
              }
            },
          },
          loading.signal,
        );
        if (cancelled) {
          instance.dispose();
          return;
        }
        controller = instance;
      } catch (cause) {
        if (cancelled) return;
        console.error("Galaxy initialization failed", cause);
        error.value = "无法初始化 3D 场景, 请确认模型资源可访问、数据有效且浏览器支持 WebGL 2 并启用硬件加速。";
      }
    },
    { immediate: true, flush: "post" },
  );

  // 重建当前快照; 旧资源由 watch 清理后再创建新实例.
  function retry(): void {
    attempt.value++;
  }

  // 通过运行时唯一选择入口返回全景, 界面状态由回调同步.
  function overview(): void {
    controller?.overview();
  }

  // 从当前选择解析 Star, 列表只传递稳定行星标识.
  function selectPlanet(planetId: string): void {
    if (selection.value) controller?.selectPlanet(selection.value.star.id, planetId);
  }

  // 聚焦当前行星, 不把 Three.js 对象暴露到组件响应式树.
  function focusPlanet(): void {
    const value = selection.value;
    if (value?.planet) controller?.focusPlanet(value.star.id, value.planet.id);
  }

  return {
    selectedStar: computed(() => selection.value?.star ?? null),
    selectedPlanet: computed(() => selection.value?.planet ?? null),
    fps,
    error,
    retry,
    overview,
    selectPlanet,
    focusPlanet,
  };
}
