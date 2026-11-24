import { defineConfig } from "vitest/config";
import react from "@vitejs/plugin-react";

declare const process: { env: Record<string, string | undefined> };

const backend = process.env.MP_WEB_BACKEND || "http://127.0.0.1:8080";

export default defineConfig({
  plugins: [react()],
  server: {
    proxy: {
      // no changeOrigin: keeps Host == Origin hostname for the bridge's check
      "/api": { target: backend },
      "/ws": { target: backend, ws: true },
    },
  },
  build: { outDir: "dist" },
  test: { environment: "node" },
});
