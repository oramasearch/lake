// SPDX-License-Identifier: MIT

import type { Screen } from "@/routes/types";
import { lazyPage } from "@/lib/lazyPage";

const Workspaces = lazyPage(() => import("./WorkspacesPage"));

export const screens: Screen[] = [
  { path: "/workspaces", title: "Workspaces", section: "ship", nav: true, icon: "api", component: Workspaces },
];
