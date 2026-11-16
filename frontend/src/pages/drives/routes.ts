// SPDX-License-Identifier: MIT

/** Data → Drives (lake#561, docs/ui/drives.md): workspace pages, served under `/w/<id>/`. The list
 * is `/drives` (its Storage tab `?tab=storage`, lake#656); one drive is
 * `/drives/drive?id=<id>&folder=<folder>`, the address style of lake's other detail pages.
 * `/storage` is Go to's way to the Storage tab. */
import type { Screen } from "@/routes/types";
import { lazyPage } from "@/lib/lazyPage";

export const screens: Screen[] = [
  { path: "/drives", title: "Drives", section: "data", nav: true, icon: "repository", workspace: true, owns: ["/drives/drive", "/storage"], component: lazyPage(() => import("./DrivesPage")) },
  { path: "/drives/drive", title: "Drive", section: "data", workspace: true, component: lazyPage(() => import("./DrivePage")) },
  { path: "/storage", title: "Storage", section: "data", workspace: true, component: lazyPage(() => import("./StorageRedirect")) },
];
