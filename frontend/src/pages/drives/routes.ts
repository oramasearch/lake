/** Data → Drives (lake#561, docs/ui/drives.md): workspace pages, served under `/w/<id>/`. The list
 * is `/drives`; one drive is `/drives/drive?id=<id>&folder=<folder>`, the address style of lake's
 * other detail pages. */
import type { Screen } from "@/routes/types";
import { lazyPage } from "@/lib/lazyPage";

export const screens: Screen[] = [
  { path: "/drives", title: "Drives", section: "data", nav: true, icon: "repository", workspace: true, owns: ["/drives/drive"], component: lazyPage(() => import("./DrivesPage")) },
  { path: "/drives/drive", title: "Drive", section: "data", workspace: true, component: lazyPage(() => import("./DrivePage")) },
];
