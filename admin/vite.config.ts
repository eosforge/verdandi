import vue from "@vitejs/plugin-vue";
import { defineConfig } from "vite";

export default defineConfig({
  plugins: [vue()],
  build: {
    rolldownOptions: {
      output: {
        codeSplitting: {
          groups: [
            {
              // 保持 Three.js 按需分包, 避免首页入口静态包含整个渲染器.
              name(id) {
                const path = id.replaceAll("\\", "/");
                if (path.endsWith("/three.core.js")) return "three-core";
                if (path.includes("/three/")) return "three-renderer";
                return undefined;
              },
            },
          ],
        },
      },
    },
  },
});
