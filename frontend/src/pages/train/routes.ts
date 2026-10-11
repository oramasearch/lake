import type { Screen } from "@/routes/types";
import { lazyPage } from "@/lib/lazyPage";

export const screens: Screen[] = [
  // Training and improving the agents that learn (lake#653); designing them is Design → Agents.
  { path: "/train/agents", title: "Agents", section: "train", nav: true, icon: "agent", workspace: true, owns: ["/train/agent"], component: lazyPage(() => import("./AgentsPage")) },
  { path: "/train/data", title: "Data", section: "data", nav: true, icon: "metering", workspace: true, owns: ["/train/dataset", "/train/query"], component: lazyPage(() => import("./DataPage")) },
  { path: "/train/runs", title: "Runs", section: "train", nav: true, icon: "metering", workspace: true, owns: ["/train/run"], component: lazyPage(() => import("./RunsPage")) },
  // Training a model (lake#599): every model with its versions, and one model's training page.
  { path: "/train/models", title: "Models", section: "train", nav: true, icon: "metering", workspace: true, owns: ["/train/model"], component: lazyPage(() => import("./ModelsPage")) },
  { path: "/train/model", title: "Model", section: "train", workspace: true, component: lazyPage(() => import("./ModelPage")) },
  { path: "/train/agent", title: "Agent", section: "train", workspace: true, component: lazyPage(() => import("./AgentPage")) },
  // The Agent Factory moved to Design → Agents (lake#653); its old address redirects.
  { path: "/train/agents/new", title: "Agent Factory", section: "design", workspace: true, component: lazyPage(() => import("./FactoryRedirect")) },
  { path: "/train/dataset", title: "Dataset", section: "data", workspace: true, component: lazyPage(() => import("./DatasetPage")) },
  { path: "/train/query", title: "Query", section: "data", workspace: true, component: lazyPage(() => import("./QueryPage")) },
  { path: "/train/run", title: "Run", section: "train", workspace: true, component: lazyPage(() => import("./RunPage")) },
];
