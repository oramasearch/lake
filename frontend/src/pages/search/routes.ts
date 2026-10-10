import type { Screen } from "@/routes/types";
import { stub } from "@/components/ScreenStub";

export const screens: Screen[] = [
  { path: "/search", title: "Search", section: "top", nav: true, icon: "search", component: stub("Search") },
];
