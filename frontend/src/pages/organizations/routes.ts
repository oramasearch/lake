import type { Screen } from "@/routes/types";
import { stub } from "@/components/ScreenStub";

export const screens: Screen[] = [
  { path: "/organizations", title: "Organizations", section: "organization", nav: true, icon: "organization", owns: ["/organization", "/teams", "/team", "/members", "/member", "/invitations", "/grants", "/access", "/sso"], component: stub("Organizations") },
  { path: "/organization", title: "Organization", section: "organization", component: stub("Organization") },
  { path: "/team", title: "Team", section: "organization", component: stub("Team") },
  { path: "/member", title: "Member", section: "organization", component: stub("Member") },
  { path: "/sso", title: "Enterprise SSO", section: "organization", component: stub("Enterprise SSO") },
  { path: "/teams", title: "Teams", section: "organization", component: stub("Teams") },
  { path: "/members", title: "Members", section: "organization", component: stub("Members") },
  { path: "/invitations", title: "Invitations", section: "organization", component: stub("Invitations") },
  { path: "/grants", title: "Direct Grants", section: "organization", component: stub("Direct Grants") },
  { path: "/access", title: "Effective Access", section: "organization", component: stub("Effective Access") },
  { path: "/repository/access", title: "Effective Access", section: "organization", component: stub("Effective Access") },
  { path: "/organization/access", title: "Effective Access", section: "organization", component: stub("Effective Access") },
  { path: "/team/access", title: "Effective Access", section: "organization", component: stub("Effective Access") },
];
