/** Data → Connections (lake#651, docs/ui/connections.md): workspace pages, served under `/w/<id>/`.
 * The list is `/connections`; one connection is `/connections/connection?name=<name>`, the address
 * style of lake's other detail pages. */
import type { Screen } from "@/routes/types";
import { lazyPage } from "@/lib/lazyPage";

export const screens: Screen[] = [
  { path: "/connections", title: "Connections", section: "data", nav: true, icon: "api", workspace: true, owns: ["/connections/connection"], component: lazyPage(() => import("./ConnectionsPage")) },
  { path: "/connections/connection", title: "Connection", section: "data", workspace: true, component: lazyPage(() => import("./ConnectionPage")) },
];
