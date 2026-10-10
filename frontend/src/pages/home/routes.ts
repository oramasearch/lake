import type { Screen } from "@/routes/types";
import { lazyPage } from "@/lib/lazyPage";

export const screens: Screen[] = [
  { path: "/", title: "Home", section: "top", nav: true, icon: "home", audience: ["visitor", "account", "operator"], component: lazyPage(() => import("./HomePage")) },
  { path: "*", title: "Not Found", section: "none", audience: ["visitor", "account", "operator"], component: lazyPage(() => import("./NotFoundPage")) },
];
