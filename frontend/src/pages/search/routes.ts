import type { Screen } from "@/routes/types";
import { lazyPage } from "@/lib/lazyPage";

export const screens: Screen[] = [
  { path: "/search", title: "Search", section: "top", nav: true, icon: "search", component: lazyPage(() => import("./SearchPage")) },
];
