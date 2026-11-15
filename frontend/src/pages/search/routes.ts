// SPDX-License-Identifier: MIT

import type { Screen } from "@/routes/types";
import { lazyPage } from "@/lib/lazyPage";

export const screens: Screen[] = [
  { path: "/search", title: "Search", section: "data", nav: true, icon: "search", component: lazyPage(() => import("./SearchPage")) },
];
