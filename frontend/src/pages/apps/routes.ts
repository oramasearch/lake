// SPDX-License-Identifier: MIT

import type { Screen } from "@/routes/types";
import { lazyPage } from "@/lib/lazyPage";

export const screens: Screen[] = [
  { path: "/ship/packages", title: "Packages", section: "ship", nav: true, icon: "repository", workspace: true, component: lazyPage(() => import("./PackagesPage")) },
  { path: "/ship/apps", title: "Apps", section: "ship", nav: true, icon: "api", workspace: true, owns: ["/ship/app"], component: lazyPage(() => import("./AppsPage")) },
  { path: "/ship/app", title: "App", section: "ship", workspace: true, component: lazyPage(() => import("./AppPage")) },
  { path: "/deployments", title: "Deployments", section: "ship", nav: true, icon: "api", component: lazyPage(() => import("./DeploymentsPage")) },
  { path: "/environments", title: "Environments", section: "top", component: lazyPage(() => import("./EnvironmentsPage")) },
  { path: "/environment", title: "Environment", section: "top", component: lazyPage(() => import("./EnvironmentPage")) },
  { path: "/logs", title: "Logs", section: "top", component: lazyPage(() => import("./LogsPage")) },
  { path: "/exports", title: "Export", section: "top", component: lazyPage(() => import("./ExportsPage")) },
];
