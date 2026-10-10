import type { Screen } from "@/routes/types";
import { stub } from "@/components/ScreenStub";

export const screens: Screen[] = [
  { path: "/ask", title: "Ask Orama", section: "none", workspace: true, component: stub("Ask Orama") },
];
