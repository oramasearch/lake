import type { Screen } from "@/routes/types";
import { lazyPage } from "@/lib/lazyPage";

const Projects = lazyPage(() => import("./ProjectsPage"));
const Project = lazyPage(() => import("./ProjectPage"));
const Pipeline = lazyPage(() => import("./PipelinePage"));
const Opportunities = lazyPage(() => import("./OpportunitiesPage"));
const Solutions = lazyPage(() => import("./SolutionsPage"));
const Solution = lazyPage(() => import("./SolutionPage"));
const Methodologies = lazyPage(() => import("./MethodologiesPage"));
const Methodology = lazyPage(() => import("./MethodologyPage"));
const AgentLibrary = lazyPage(() => import("./AgentLibraryPage"));
const Icons = lazyPage(() => import("./IconsPage"));

export const screens: Screen[] = [
  { path: "/design/projects", title: "Projects", section: "design", nav: true, icon: "audit", workspace: true, owns: ["/design/project", "/design/pipeline"], component: Projects },
  { path: "/design/opportunities", title: "Opportunities", section: "design", nav: true, icon: "search", workspace: true, owns: ["/design/discover"], component: Opportunities },
  { path: "/design/solutions", title: "Solutions", section: "design", nav: true, icon: "audit", workspace: true, owns: ["/design/solution"], component: Solutions },
  { path: "/design/methodologies", title: "Methodologies", section: "design", nav: true, icon: "metering", workspace: true, owns: ["/design/methodology"], component: Methodologies },
  { path: "/design/agent-library", title: "Agent Library", section: "design", nav: true, icon: "agent", workspace: true, component: AgentLibrary },
  { path: "/design/project", title: "Project", section: "design", workspace: true, component: Project },
  { path: "/design/pipeline", title: "Pipeline", section: "design", workspace: true, component: Pipeline },
  { path: "/design/discover", title: "Opportunities", section: "design", workspace: true, component: Opportunities },
  { path: "/design/solution", title: "Solution", section: "design", workspace: true, component: Solution },
  { path: "/design/methodology", title: "Methodology", section: "design", workspace: true, component: Methodology },
  // The catalogue icons at every size, for review (lake#639); not in the rail, open to everyone.
  { path: "/design/icons", title: "Catalogue Icons", section: "design", audience: ["visitor", "account", "operator"], component: Icons },
];
