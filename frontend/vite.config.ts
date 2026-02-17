import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

export default defineConfig({
  plugins: [react()],
  server: {
    host: true,
    port: 5173,
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
});
