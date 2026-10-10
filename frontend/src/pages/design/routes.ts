import type { Screen } from "@/routes/types";
import { stub } from "@/components/ScreenStub";

export const screens: Screen[] = [
  { path: "/design/projects", title: "Projects", section: "design", nav: true, icon: "audit", workspace: true, owns: ["/design/project", "/design/pipeline"], component: stub("Projects") },
  { path: "/design/opportunities", title: "Opportunities", section: "design", nav: true, icon: "search", workspace: true, owns: ["/design/discover"], component: stub("Opportunities") },
  { path: "/design/solutions", title: "Solutions", section: "design", nav: true, icon: "audit", workspace: true, owns: ["/design/solution"], component: stub("Solutions") },
  { path: "/design/methodologies", title: "Methodologies", section: "design", nav: true, icon: "metering", workspace: true, owns: ["/design/methodology"], component: stub("Methodologies") },
  { path: "/design/agent-library", title: "Agent Library", section: "design", nav: true, icon: "agent", workspace: true, component: stub("Agent Library") },
  { path: "/design/project", title: "Project", section: "design", workspace: true, component: stub("Project") },
  { path: "/design/pipeline", title: "Pipeline", section: "design", workspace: true, component: stub("Pipeline") },
  { path: "/design/discover", title: "Opportunities", section: "design", workspace: true, component: stub("Opportunities") },
  { path: "/design/solution", title: "Solution", section: "design", workspace: true, component: stub("Solution") },
  { path: "/design/methodology", title: "Methodology", section: "design", workspace: true, component: stub("Methodology") },
];
