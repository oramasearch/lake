import type { Screen } from "@/routes/types";
import { stub } from "@/components/ScreenStub";

export const screens: Screen[] = [
  { path: "/ship/models", title: "Models", section: "ship", nav: true, icon: "api", workspace: true, component: stub("Models") },
  { path: "/ship/indexes", title: "Search Indexes", section: "ship", nav: true, icon: "search", workspace: true, owns: ["/ship/index"], component: stub("Search Indexes") },
  { path: "/ship/index", title: "Search Index", section: "ship", workspace: true, component: stub("Search Index") },
  { path: "/services", title: "Services", section: "ship", nav: true, icon: "api", component: stub("Services") },
];
