import type { Screen } from "@/routes/types";
import { stub } from "@/components/ScreenStub";

export const screens: Screen[] = [
  { path: "/workspaces", title: "Workspaces", section: "ship", nav: true, icon: "api", component: stub("Workspaces") },
];
