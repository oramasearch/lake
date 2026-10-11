import type { Screen } from "@/routes/types";
import { lazyPage } from "@/lib/lazyPage";

// Billing (lake#564): an account's plan, this month's bill and its invoices; one invoice; and the
// operator's price list. Only where the lake bills (`billing: true`, viewer.billing); an enterprise
// install has none of them. The operator opens one account's billing with `?customer_id=`.
export const screens: Screen[] = [
  { path: "/billing", title: "Billing", section: "organization", icon: "metering", billing: true, owns: ["/billing/invoices"], component: lazyPage(() => import("./BillingPage")) },
  { path: "/billing/invoices/:invoiceId", title: "Invoice", section: "organization", billing: true, component: lazyPage(() => import("./InvoicePage")) },
  { path: "/billing/plans", title: "Price List", section: "administration", audience: ["operator"], billing: true, component: lazyPage(() => import("./PlansPage")) },
];
