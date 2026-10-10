import type { Screen } from "@/routes/types";
import { lazyPage } from "@/lib/lazyPage";

export const screens: Screen[] = [
  { path: "/ask", title: "Ask Orama", section: "none", workspace: true, component: lazyPage(() => import("./AskPage")) },
];
