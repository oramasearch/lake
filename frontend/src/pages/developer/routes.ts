import type { Screen } from "@/routes/types";
import { stub } from "@/components/ScreenStub";

export const screens: Screen[] = [
  { path: "/developer/mcp", title: "MCP", section: "developer", nav: true, icon: "api", component: stub("MCP") },
  { path: "/docs", title: "Docs", section: "developer", nav: true, icon: "docs", audience: ["visitor", "account", "operator"], owns: ["/docs"], component: stub("Docs") },
  { path: "/docs/:page", title: "Docs", section: "developer", audience: ["visitor", "account", "operator"], component: stub("Docs") },
];
