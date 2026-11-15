// SPDX-License-Identifier: MIT

import type { Screen } from "@/routes/types";
import { lazyPage } from "@/lib/lazyPage";

const Access = lazyPage(() => import("./AccessPage"));

export const screens: Screen[] = [
  { path: "/organizations", title: "Organizations", section: "organization", nav: true, icon: "organization", owns: ["/organization", "/teams", "/team", "/members", "/member", "/invitations", "/grants", "/access", "/sso"], component: lazyPage(() => import("./OrganizationsPage")) },
  { path: "/organization", title: "Organization Settings", section: "organization", component: lazyPage(() => import("./OrganizationPage")) },
  { path: "/team", title: "Team Settings", section: "organization", component: lazyPage(() => import("./TeamPage")) },
  { path: "/member", title: "Member Settings", section: "organization", component: lazyPage(() => import("./MemberPage")) },
  { path: "/sso", title: "Enterprise SSO", section: "organization", component: lazyPage(() => import("./SsoPage")) },
  { path: "/teams", title: "Teams", section: "organization", component: lazyPage(() => import("./TeamsPage")) },
  { path: "/members", title: "Members", section: "organization", component: lazyPage(() => import("./MembersPage")) },
  { path: "/invitations", title: "Invitations", section: "organization", component: lazyPage(() => import("./InvitationsPage")) },
  { path: "/grants", title: "Direct Grants", section: "organization", component: lazyPage(() => import("./GrantsPage")) },
  { path: "/access", title: "Effective Access", section: "organization", component: Access },
  { path: "/repository/access", title: "Effective Access", section: "organization", component: Access },
  { path: "/organization/access", title: "Effective Access", section: "organization", component: Access },
  { path: "/team/access", title: "Effective Access", section: "organization", component: Access },
];
