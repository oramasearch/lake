import type { Screen } from "@/routes/types";
import { stub } from "@/components/ScreenStub";

export const screens: Screen[] = [
  { path: "/train/agents", title: "Agents", section: "train", nav: true, icon: "agent", workspace: true, owns: ["/train/agent"], component: stub("Agents") },
  { path: "/train/data", title: "Data", section: "train", nav: true, icon: "metering", workspace: true, owns: ["/train/dataset", "/train/query"], component: stub("Data") },
  { path: "/train/runs", title: "Runs", section: "train", nav: true, icon: "metering", workspace: true, owns: ["/train/run"], component: stub("Runs") },
  { path: "/train/agent", title: "Agent", section: "train", workspace: true, component: stub("Agent") },
  { path: "/train/agents/new", title: "Agent Factory", section: "train", workspace: true, component: stub("Agent Factory") },
  { path: "/train/dataset", title: "Dataset", section: "train", workspace: true, component: stub("Dataset") },
  { path: "/train/query", title: "Query", section: "train", workspace: true, component: stub("Query") },
  { path: "/train/run", title: "Run", section: "train", workspace: true, component: stub("Run") },
];
