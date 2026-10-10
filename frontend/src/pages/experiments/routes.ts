import type { Screen } from "@/routes/types";
import { stub } from "@/components/ScreenStub";

export const screens: Screen[] = [
  { path: "/train/experiments", title: "Experiments", section: "train", nav: true, icon: "metering", workspace: true, owns: ["/train/experiment"], component: stub("Experiments") },
  { path: "/train/experiment", title: "Study", section: "train", workspace: true, component: stub("Study") },
  { path: "/train/experiment/run", title: "Run Charts", section: "train", workspace: true, component: stub("Run Charts") },
  { path: "/train/experiments/compare", title: "Compare Runs", section: "train", workspace: true, component: stub("Compare Runs") },
];
