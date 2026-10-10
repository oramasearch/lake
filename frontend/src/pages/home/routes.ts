import type { Screen } from "@/routes/types";
import { stub } from "@/components/ScreenStub";

export const screens: Screen[] = [
  { path: "/", title: "Home", section: "top", nav: true, icon: "home", audience: ["visitor", "account", "operator"], component: stub("Home") },
  { path: "*", title: "Not Found", section: "none", audience: ["visitor", "account", "operator"], component: stub("Not Found") },
];
