import type { Screen } from "@/routes/types";
import { lazyPage } from "@/lib/lazyPage";

const Audit = lazyPage(() => import("./AuditPage"));
const Model = lazyPage(() => import("./ModelPage"));

// Customers and the application console open for any account, as on the server: the
// capabilities filter what each shows, and a space's admins register applications. The rail
// offers them to the operator only (routes.tsx NAV).
export const screens: Screen[] = [
  { path: "/customers", title: "Customers", section: "administration", nav: true, icon: "customers", component: lazyPage(() => import("./CustomersPage")) },
  { path: "/applications", title: "Applications", section: "administration", nav: true, icon: "api", owns: ["/application"], component: lazyPage(() => import("./ApplicationsPage")) },
  { path: "/application", title: "Application", section: "administration", component: lazyPage(() => import("./ApplicationPage")) },
  { path: "/application/users", title: "Application Users", section: "administration", component: lazyPage(() => import("./ApplicationUsersPage")) },
  { path: "/application/organizations", title: "Application Organizations", section: "administration", component: lazyPage(() => import("./ApplicationOrganizationsPage")) },
  { path: "/application/events", title: "Application Events", section: "administration", component: lazyPage(() => import("./ApplicationEventsPage")) },
  { path: "/secret-mounts", title: "Secrets", section: "organization", nav: true, icon: "secrets", owns: ["/secrets", "/encryption-keys"], component: lazyPage(() => import("./SecretMountsPage")) },
  { path: "/secrets", title: "Secrets", section: "organization", component: lazyPage(() => import("./SecretsPage")) },
  { path: "/encryption-keys", title: "Encryption Keys", section: "organization", component: lazyPage(() => import("./EncryptionKeysPage")) },
  { path: "/metering", title: "Repository Usage", section: "organization", component: lazyPage(() => import("./MeteringPage")) },
  { path: "/activity", title: "Activity", section: "organization", nav: true, icon: "audit", audience: ["account"], component: Audit },
  { path: "/audit", title: "Audit", section: "administration", nav: true, icon: "audit", audience: ["operator"], component: Audit },
  { path: "/transparency", title: "Transparency Monitor", section: "none", audience: ["visitor", "account", "operator"], component: lazyPage(() => import("./TransparencyPage")) },
  { path: "/models/:namespace/:id", title: "Model", section: "top", audience: ["visitor", "account", "operator"], component: Model },
  { path: "/models/:id", title: "Model", section: "top", audience: ["visitor", "account", "operator"], component: Model },
];
