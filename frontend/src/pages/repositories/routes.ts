import type { Screen } from "@/routes/types";
import { stub } from "@/components/ScreenStub";

export const screens: Screen[] = [
  { path: "/repositories", title: "Repositories", section: "top", nav: true, icon: "repository", audience: ["visitor", "account", "operator"], owns: ["/repository", "/file", "/changes", "/change", "/issues", "/issue", "/releases", "/webhooks", "/protection", "/conflicts", "/artifacts", "/artifact", "/exports", "/drive", "/environments", "/environment", "/logs", "/models", "/datasets"], component: stub("Repositories") },
  { path: "/repository", title: "Repository", section: "top", audience: ["visitor", "account", "operator"], component: stub("Repository") },
  { path: "/repository/tree", title: "Files", section: "top", audience: ["visitor", "account", "operator"], component: stub("Files") },
  { path: "/file", title: "File", section: "top", audience: ["visitor", "account", "operator"], component: stub("File") },
  { path: "/changes", title: "Change Log", section: "top", audience: ["visitor", "account", "operator"], component: stub("Change Log") },
  { path: "/change", title: "Change", section: "top", audience: ["visitor", "account", "operator"], component: stub("Change") },
  { path: "/issues", title: "Issues", section: "top", audience: ["visitor", "account", "operator"], component: stub("Issues") },
  { path: "/issue", title: "Issue", section: "top", audience: ["visitor", "account", "operator"], component: stub("Issue") },
  { path: "/releases", title: "Releases", section: "top", audience: ["visitor", "account", "operator"], component: stub("Releases") },
  { path: "/webhooks", title: "Webhooks", section: "top", audience: ["visitor", "account", "operator"], component: stub("Webhooks") },
  { path: "/protection", title: "Branch Protection", section: "top", audience: ["visitor", "account", "operator"], component: stub("Branch Protection") },
  { path: "/conflicts", title: "Conflicts", section: "top", audience: ["visitor", "account", "operator"], component: stub("Conflicts") },
  { path: "/artifacts", title: "Build Results", section: "top", audience: ["visitor", "account", "operator"], component: stub("Build Results") },
  { path: "/artifact", title: "Build Result", section: "top", audience: ["visitor", "account", "operator"], component: stub("Build Result") },
  { path: "/drive", title: "Drive", section: "top", audience: ["visitor", "account", "operator"], component: stub("Drive") },
  { path: "/drive/preview", title: "Preview", section: "top", audience: ["visitor", "account", "operator"], component: stub("Preview") },
  { path: "/drive/history", title: "History", section: "top", audience: ["visitor", "account", "operator"], component: stub("History") },
  { path: "/drive/history/diff", title: "Compare Versions", section: "top", audience: ["visitor", "account", "operator"], component: stub("Compare Versions") },
  { path: "/drive/conflict", title: "Conflict", section: "top", audience: ["visitor", "account", "operator"], component: stub("Conflict") },
  { path: "/drive/share", title: "Sharing", section: "top", audience: ["visitor", "account", "operator"], component: stub("Sharing") },
  { path: "/drive/share/link", title: "Share Link", section: "top", audience: ["visitor", "account", "operator"], component: stub("Share Link") },
  { path: "/shared-with-me", title: "Shared with Me", section: "top", nav: true, icon: "repository", audience: ["account"], owns: ["/share"], component: stub("Shared with Me") },
  { path: "/share/:token", title: "Shared Folder", section: "none", audience: ["visitor", "account", "operator"], bare: true, component: stub("Shared Folder") },
  { path: "/share/:token/preview", title: "Shared File", section: "none", audience: ["visitor", "account", "operator"], bare: true, component: stub("Shared File") },
];
