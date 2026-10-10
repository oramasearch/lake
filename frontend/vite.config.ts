import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";
import tailwindcss from "@tailwindcss/vite";
import { fileURLToPath, URL } from "node:url";

// One process in production: lake embeds dist/ into its binary (tools/ui/embed-frontend.mjs, run by CMakeLists.txt),
// serves the hashed files under /ui/ immutable and index.html for every page route. In
// development, Vite proxies everything that is not the app to a local lake (LAKE_ORIGIN,
// default http://127.0.0.1:4900).
const lake = process.env.LAKE_ORIGIN ?? "http://127.0.0.1:4900";
// Prefixes are matched as regular expressions ending at a path boundary, so "/w/" never claims
// "/workspaces". A workspace's API calls (/w/<id>/v1, /w/<id>/mcp) go to lake; its pages stay here.
const proxied = [
  "/v1", "/w/[^/]+/(v1|mcp)", "/session", "/identity", "/apps", "/mcp", "/fonts", "/assets", "/drive/download",
  "/drive/preview", "/share", "/file/download", "/exports/download", "/llms.txt", "/llms-full.txt",
  "/skills", "/healthz", "/readyz", "/metrics",
];

export default defineConfig({
  base: "/",
  plugins: [react(), tailwindcss()],
  resolve: { alias: { "@": fileURLToPath(new URL("./src", import.meta.url)) } },
  server: {
    proxy: Object.fromEntries(proxied.map((path) => [`^${path}(/|$|\\?)`, { target: lake, changeOrigin: false }])),
  },
  build: { assetsDir: "ui", chunkSizeWarningLimit: 900 },
});
