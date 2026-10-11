import type { Screen } from "@/routes/types";
import { lazyPage } from "@/lib/lazyPage";

// Ask Orama is the home (`/`, lake#600). The page that was `/ask` is gone; its links, with or
// without a workspace prefix, open the home with their query.
export const screens: Screen[] = [
  { path: "/ask", title: "Ask Orama", section: "none", audience: ["visitor", "account", "operator"], component: lazyPage(() => import("./AskRedirect")) },
  { path: "/w/:workspaceId/ask", title: "Ask Orama", section: "none", audience: ["visitor", "account", "operator"], component: lazyPage(() => import("./AskRedirect")) },
];
