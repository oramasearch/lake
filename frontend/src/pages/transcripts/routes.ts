/** Data → Transcripts (lake#561, docs/ui/transcripts.md): workspace pages, served under
 * `/w/<id>/`. The list is `/transcripts` (`?q=` searches); one transcript is `/transcript?id=<id>`,
 * the address style of lake's other detail pages. */
import type { Screen } from "@/routes/types";
import { lazyPage } from "@/lib/lazyPage";

export const screens: Screen[] = [
  { path: "/transcripts", title: "Transcripts", section: "data", nav: true, icon: "audit", workspace: true, owns: ["/transcript"], component: lazyPage(() => import("./TranscriptsPage")) },
  { path: "/transcript", title: "Transcript", section: "data", workspace: true, component: lazyPage(() => import("./TranscriptPage")) },
];
