import type { Screen } from "@/routes/types";
import { lazyPage } from "@/lib/lazyPage";

const docs = lazyPage(() => import("./DocsPage"));

export const screens: Screen[] = [
  { path: "/developer/mcp", title: "MCP", section: "developer", nav: true, icon: "api", component: lazyPage(() => import("./McpPage")) },
  { path: "/docs", title: "Docs", section: "developer", nav: true, icon: "docs", audience: ["visitor", "account", "operator"], owns: ["/docs"], component: docs },
  { path: "/docs/:page", title: "Docs", section: "developer", audience: ["visitor", "account", "operator"], component: docs },
];
