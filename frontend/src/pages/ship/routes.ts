import type { Screen } from "@/routes/types";
import { lazyPage } from "@/lib/lazyPage";

export const screens: Screen[] = [
  { path: "/ship/models", title: "Models", section: "ship", nav: true, icon: "api", workspace: true, component: lazyPage(() => import("./ModelsPage")) },
  { path: "/ship/indexes", title: "Search Indexes", section: "ship", nav: true, icon: "search", workspace: true, owns: ["/ship/index"], component: lazyPage(() => import("./IndexesPage")) },
  { path: "/ship/index", title: "Search Index", section: "ship", workspace: true, component: lazyPage(() => import("./IndexPage")) },
  { path: "/services", title: "Services", section: "ship", nav: true, icon: "api", component: lazyPage(() => import("./ServicesPage")) },
];
