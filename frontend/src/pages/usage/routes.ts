import type { Screen } from "@/routes/types";
import { lazyPage } from "@/lib/lazyPage";

// Usage over time (lake#564): the dashboard for a person (and, with `?customer_id=`, the operator's
// view of one account), and the operator's view across every account. `/metering` stays the
// repository usage by hour, linked from the dashboard.
export const screens: Screen[] = [
  { path: "/usage", title: "Usage", section: "organization", nav: true, icon: "metering", owns: ["/metering"], component: lazyPage(() => import("./UsagePage")) },
  { path: "/usage/accounts", title: "Every Account", section: "administration", audience: ["operator"], component: lazyPage(() => import("./AccountsPage")) },
];
