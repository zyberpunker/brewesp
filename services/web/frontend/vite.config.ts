import path from "node:path";

import tailwindcss from "@tailwindcss/vite";
import react from "@vitejs/plugin-react";
import { defineConfig } from "vite";

export default defineConfig({
  plugins: [react(), tailwindcss()],
  define: {
    "process.env.NODE_ENV": JSON.stringify("production"),
  },
  resolve: {
    alias: {
      "@": path.resolve(__dirname, "./src"),
    },
  },
  build: {
    outDir: "dist/react",
    emptyOutDir: true,
    lib: {
      entry: path.resolve(__dirname, "src/device-detail/main.tsx"),
      formats: ["es"],
      fileName: () => "device-detail.js",
      cssFileName: "device-detail",
    },
  },
});
