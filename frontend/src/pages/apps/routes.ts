import type { Screen } from "@/routes/types";
import { stub } from "@/components/ScreenStub";

export const screens: Screen[] = [
  { path: "/ship/packages", title: "Packages", section: "ship", nav: true, icon: "repository", workspace: true, component: stub("Packages") },
  { path: "/ship/apps", title: "Apps", section: "ship", nav: true, icon: "api", workspace: true, owns: ["/ship/app"], component: stub("Apps") },
  { path: "/ship/app", title: "App", section: "ship", workspace: true, component: stub("App") },
  { path: "/deployments", title: "Deployments", section: "ship", nav: true, icon: "api", component: stub("Deployments") },
  { path: "/environments", title: "Environments", section: "top", component: stub("Environments") },
  { path: "/environment", title: "Environment", section: "top", component: stub("Environment") },
  { path: "/logs", title: "Logs", section: "top", component: stub("Logs") },
  { path: "/exports", title: "Export", section: "top", component: stub("Export") },
];
