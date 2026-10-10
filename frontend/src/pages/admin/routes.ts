import type { Screen } from "@/routes/types";
import { stub } from "@/components/ScreenStub";

export const screens: Screen[] = [
  { path: "/customers", title: "Customers", section: "administration", nav: true, icon: "customers", audience: ["operator"], component: stub("Customers") },
  { path: "/applications", title: "Applications", section: "administration", nav: true, icon: "api", audience: ["operator"], owns: ["/application"], component: stub("Applications") },
  { path: "/application", title: "Application", section: "administration", audience: ["operator"], component: stub("Application") },
  { path: "/application/users", title: "Users", section: "administration", audience: ["operator"], component: stub("Users") },
  { path: "/application/organizations", title: "Organizations", section: "administration", audience: ["operator"], component: stub("Organizations") },
  { path: "/application/events", title: "Events", section: "administration", audience: ["operator"], component: stub("Events") },
  { path: "/secret-mounts", title: "Secrets", section: "organization", nav: true, icon: "secrets", owns: ["/secrets", "/encryption-keys"], component: stub("Secrets") },
  { path: "/secrets", title: "Secrets", section: "organization", component: stub("Secrets") },
  { path: "/encryption-keys", title: "Encryption Keys", section: "organization", component: stub("Encryption Keys") },
  { path: "/metering", title: "Usage", section: "organization", nav: true, icon: "metering", component: stub("Usage") },
  { path: "/activity", title: "Activity", section: "organization", nav: true, icon: "audit", audience: ["account"], component: stub("Activity") },
  { path: "/audit", title: "Audit", section: "administration", nav: true, icon: "audit", audience: ["operator"], component: stub("Audit") },
  { path: "/transparency", title: "Transparency Monitor", section: "none", audience: ["visitor", "account", "operator"], component: stub("Transparency Monitor") },
  { path: "/models/:namespace/:id", title: "Model", section: "top", audience: ["visitor", "account", "operator"], component: stub("Model") },
  { path: "/models/:id", title: "Model", section: "top", audience: ["visitor", "account", "operator"], component: stub("Model") },
];
