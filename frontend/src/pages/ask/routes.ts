import type { Screen } from "@/routes/types";
import { lazyPage } from "@/lib/lazyPage";

// Ask Orama has its own page again (`/ask`, lake#1128): Quickstart is the home. A workspace-prefixed
// link (`/w/<id>/ask`) and an old home link with a conversation (`/?conversation=`, see
// pages/home/HomePage.tsx) open it with their query.
export const screens: Screen[] = [
  { path: "/ask", title: "Ask Orama", section: "home", audience: ["account"], component: lazyPage(() => import("./AskPage")) },
  { path: "/w/:workspaceId/ask", title: "Ask Orama", section: "none", audience: ["visitor", "account", "operator"], component: lazyPage(() => import("./AskRedirect")) },
];
