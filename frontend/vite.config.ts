import { defineConfig, loadEnv } from "vite";
import react from "@vitejs/plugin-react";

export default defineConfig(({ mode }) => {
  const env = loadEnv(mode, process.cwd(), "");
  const backendHttpTarget = env.VITE_BACKEND_HTTP_URL || "http://127.0.0.1:18080";
  const backendWsTarget = env.VITE_BACKEND_WS_URL || "ws://127.0.0.1:18080";

  return {
    plugins: [react()],
    server: {
      host: true,
      port: 5173,
      proxy: {
        "/api": {
          target: backendHttpTarget,
          changeOrigin: true,
        },
        "/ws": {
          target: backendWsTarget,
          changeOrigin: true,
          ws: true,
        },
      },
    },
    build: {
      outDir: "dist",
      assetsDir: "",
      sourcemap: false,
      rollupOptions: {
        output: {
          entryFileNames: "app-[hash].js",
          chunkFileNames: "chunk-[name]-[hash].js",
          assetFileNames: "asset-[name]-[hash][extname]",
        },
      },
    },
  };
});
