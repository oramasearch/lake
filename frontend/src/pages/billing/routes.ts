import type { Screen } from "@/routes/types";
import { lazyPage } from "@/lib/lazyPage";

// Billing (lake#564, lake#635): this month's usage and its cost and the invoices; one invoice; and the
// operator's price list. Only where the lake bills (`billing: true`, viewer.billing); an enterprise
// install has none of them. The operator opens one account's billing with `?customer_id=`.
export const screens: Screen[] = [
  { path: "/billing", title: "Billing", section: "organization", icon: "metering", billing: true, owns: ["/billing/invoices", "/billing/add-funds"], component: lazyPage(() => import("./BillingPage")) },
  // Add funds (lake#1114): the one page the balance's emails, banner and 402 answers link to.
  { path: "/billing/add-funds", title: "Add Funds", section: "organization", billing: true, component: lazyPage(() => import("./AddFundsPage")) },
  { path: "/billing/invoices/:invoiceId", title: "Invoice", section: "organization", billing: true, component: lazyPage(() => import("./InvoicePage")) },
  { path: "/billing/plans", title: "Price List", section: "administration", audience: ["operator"], billing: true, component: lazyPage(() => import("./PriceListPage")) },
  // Coupon codes for ads (lake#1156): the operator's, where the lake bills.
  { path: "/coupons", title: "Coupons", section: "administration", audience: ["operator"], billing: true, component: lazyPage(() => import("../admin/CouponsPage")) },
];
