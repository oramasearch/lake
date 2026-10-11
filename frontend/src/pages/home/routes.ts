import type { Screen } from "@/routes/types";
import { lazyPage } from "@/lib/lazyPage";

export const screens: Screen[] = [
  // A signed-in person's home is Ask Orama; the operator and visitors see the landing (lake#600).
  { path: "/", title: "Home", section: "top", nav: true, icon: "home", audience: ["visitor", "account", "operator"], component: lazyPage(() => import("./HomePage")) },
  // The whole platform at a glance, the Command Center that was the home before lake#600.
  { path: "/dashboard", title: "Dashboard", section: "home", audience: ["account"], component: lazyPage(() => import("./CommandCenter")) },
  { path: "*", title: "Not Found", section: "none", audience: ["visitor", "account", "operator"], component: lazyPage(() => import("./NotFoundPage")) },
];
