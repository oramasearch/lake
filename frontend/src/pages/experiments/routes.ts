import type { Screen } from "@/routes/types";
import { lazyPage } from "@/lib/lazyPage";

export const screens: Screen[] = [
  { path: "/train/experiments", title: "Experiments", section: "train", nav: true, icon: "metering", workspace: true, owns: ["/train/experiment"], component: lazyPage(() => import("./ExperimentsPage")) },
  { path: "/train/experiment", title: "Study", section: "train", workspace: true, component: lazyPage(() => import("./StudyPage")) },
  { path: "/train/experiment/run", title: "Run Charts", section: "train", workspace: true, component: lazyPage(() => import("./RunPage")) },
  { path: "/train/experiments/compare", title: "Compare Runs", section: "train", workspace: true, component: lazyPage(() => import("./ComparePage")) },
];
