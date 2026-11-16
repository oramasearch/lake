// SPDX-License-Identifier: MIT

/** Network (lake#729, docs/ui/network.md): a workspace's connections, developer access, private
 * networks, egress and DNS, the effective policy and its activity. Workspace pages, served under
 * `/w/<id>/`; `/network` opens the overview. */
import type { Screen } from "@/routes/types";
import { lazyPage } from "@/lib/lazyPage";
import { ACCESS_PATH, ACTIVITY_PATH, CONNECTIONS_PATH, EGRESS_PATH, NETWORK_PATH, OVERVIEW_PATH, PEERS_PATH, POLICIES_PATH } from "./network";

export const screens: Screen[] = [
  { path: NETWORK_PATH, title: "Network", section: "network", workspace: true, component: lazyPage(() => import("./NetworkRedirect")) },
  { path: OVERVIEW_PATH, title: "Network", section: "network", nav: "Overview", icon: "api", workspace: true, component: lazyPage(() => import("./OverviewPage")) },
  { path: CONNECTIONS_PATH, title: "Connections", section: "network", nav: true, icon: "api", workspace: true, component: lazyPage(() => import("./ConnectionsPage")) },
  { path: ACCESS_PATH, title: "Developer Access", section: "network", nav: true, icon: "api", workspace: true, component: lazyPage(() => import("./AccessPage")) },
  { path: PEERS_PATH, title: "Private Networks", section: "network", nav: true, icon: "api", workspace: true, component: lazyPage(() => import("./PeersPage")) },
  { path: EGRESS_PATH, title: "Egress and DNS", section: "network", nav: true, icon: "api", workspace: true, component: lazyPage(() => import("./EgressPage")) },
  { path: POLICIES_PATH, title: "Policies", section: "network", nav: true, icon: "audit", workspace: true, component: lazyPage(() => import("./PoliciesPage")) },
  { path: ACTIVITY_PATH, title: "Network Activity", section: "network", nav: "Activity", icon: "audit", workspace: true, component: lazyPage(() => import("./ActivityPage")) },
];
