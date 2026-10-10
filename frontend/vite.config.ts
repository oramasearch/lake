import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";
import tailwindcss from "@tailwindcss/vite";
import { fileURLToPath, URL } from "node:url";

// One process in production: lake embeds dist/ into its binary (cmake/embed_frontend.cmake),
// serves the hashed files under /ui/ immutable and index.html for every page route. In
// development, Vite proxies everything that is not the app to a local lake (LAKE_ORIGIN,
// default http://127.0.0.1:4900).
const lake = process.env.LAKE_ORIGIN ?? "http://127.0.0.1:4900";
const proxied = [
  "/v1", "/w", "/session", "/identity", "/apps", "/mcp", "/fonts", "/assets", "/drive/download",
  "/drive/preview", "/share", "/file/download", "/exports/download", "/llms.txt", "/llms-full.txt",
  "/skills", "/healthz", "/readyz", "/metrics",
];

export default defineConfig({
  base: "/",
  plugins: [react(), tailwindcss()],
  resolve: { alias: { "@": fileURLToPath(new URL("./src", import.meta.url)) } },
  server: { proxy: Object.fromEntries(proxied.map((path) => [path, { target: lake, changeOrigin: false }])) },
  build: { assetsDir: "ui", chunkSizeWarningLimit: 900 },
});
