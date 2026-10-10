import type { Audience, Screen } from "@/routes/types";
import { lazyPage } from "@/lib/lazyPage";
import { stub } from "@/components/ScreenStub";

const ALL: Audience[] = ["visitor", "account", "operator"];
/** Drive, webhooks and branch protection need sign-in: their capabilities have no anonymous read. */
const SIGNED_IN: Audience[] = ["account", "operator"];

export const screens: Screen[] = [
  { path: "/repositories", title: "Repositories", section: "top", nav: true, icon: "repository", audience: ALL, owns: ["/repository", "/file", "/changes", "/change", "/issues", "/issue", "/releases", "/webhooks", "/protection", "/conflicts", "/artifacts", "/artifact", "/exports", "/drive", "/environments", "/environment", "/logs", "/models", "/datasets"], component: lazyPage(() => import("./RepositoriesPage")) },
  { path: "/repository", title: "Repository", section: "top", audience: ALL, component: lazyPage(() => import("./RepositoryPage")) },
  { path: "/repository/tree", title: "Files", section: "top", audience: ALL, component: lazyPage(() => import("./TreePage")) },
  { path: "/file", title: "File", section: "top", audience: ALL, component: lazyPage(() => import("./FilePage")) },
  { path: "/changes", title: "Change Log", section: "top", audience: ALL, component: lazyPage(() => import("./ChangesPage")) },
  { path: "/change", title: "Change", section: "top", audience: ALL, component: lazyPage(() => import("./ChangePage")) },
  { path: "/issues", title: "Issues", section: "top", audience: ALL, component: lazyPage(() => import("./IssuesPage")) },
  { path: "/issue", title: "Issue", section: "top", audience: ALL, component: lazyPage(() => import("./IssuePage")) },
  { path: "/releases", title: "Releases", section: "top", audience: ALL, component: lazyPage(() => import("./ReleasesPage")) },
  { path: "/webhooks", title: "Webhooks", section: "top", audience: SIGNED_IN, component: lazyPage(() => import("./WebhooksPage")) },
  { path: "/protection", title: "Branch Protection", section: "top", audience: SIGNED_IN, component: lazyPage(() => import("./ProtectionPage")) },
  { path: "/conflicts", title: "Conflicts", section: "top", audience: ALL, component: lazyPage(() => import("./ConflictsPage")) },
  { path: "/artifacts", title: "Build Results", section: "top", audience: ALL, component: lazyPage(() => import("./ArtifactsPage")) },
  { path: "/artifact", title: "Build Result", section: "top", audience: ALL, component: lazyPage(() => import("./ArtifactPage")) },
  { path: "/drive", title: "Drive", section: "top", audience: SIGNED_IN, component: lazyPage(() => import("./DrivePage")) },
  { path: "/drive/preview", title: "Preview", section: "top", audience: SIGNED_IN, component: lazyPage(() => import("./DrivePreviewPage")) },
  { path: "/drive/history", title: "History", section: "top", audience: SIGNED_IN, component: lazyPage(() => import("./DriveHistoryPage")) },
  { path: "/drive/history/diff", title: "Compare Versions", section: "top", audience: SIGNED_IN, component: lazyPage(() => import("./DriveDiffPage")) },
  { path: "/drive/conflict", title: "Conflict", section: "top", audience: SIGNED_IN, component: lazyPage(() => import("./DriveConflictPage")) },
  { path: "/drive/share", title: "Sharing", section: "top", audience: SIGNED_IN, component: lazyPage(() => import("./DriveSharePage")) },
  { path: "/drive/share/link", title: "Share Link", section: "top", audience: SIGNED_IN, component: lazyPage(() => import("./DriveShareLinkPage")) },
  { path: "/shared-with-me", title: "Shared with Me", section: "top", nav: true, icon: "repository", audience: ["account"], owns: ["/share"], component: stub("Shared with Me") },
  { path: "/share/:token", title: "Shared Folder", section: "none", audience: ALL, bare: true, component: stub("Shared Folder") },
  { path: "/share/:token/preview", title: "Shared File", section: "none", audience: ALL, bare: true, component: stub("Shared File") },
];
