/** Data → MCP Connections (lake#652, docs/ui/mcp-connections.md): workspace pages, served under
 * `/w/<id>/`. The list is `/mcp-connections`; one connection is `/mcp-connection?id=<id>`, the
 * address style of lake's other detail pages. The OAuth sign-in returns to
 * `/mcp-connections/oauth/callback`, outside any workspace: its `state` names the workspace. */
import type { Screen } from "@/routes/types";
import { lazyPage } from "@/lib/lazyPage";

export const screens: Screen[] = [
  { path: "/mcp-connections", title: "MCP Connections", section: "data", nav: true, icon: "api", workspace: true, owns: ["/mcp-connection"], component: lazyPage(() => import("./McpConnectionsPage")) },
  { path: "/mcp-connection", title: "MCP Connection", section: "data", workspace: true, component: lazyPage(() => import("./McpConnectionPage")) },
  { path: "/mcp-connections/oauth/callback", title: "Signing In", section: "data", component: lazyPage(() => import("./OAuthCallbackPage")) },
];
