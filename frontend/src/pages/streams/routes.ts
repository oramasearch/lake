import type { Screen } from "@/routes/types";
import { stub } from "@/components/ScreenStub";

export const screens: Screen[] = [
  { path: "/train/streams", title: "Streams", section: "train", nav: true, icon: "streams", workspace: true, owns: ["/train/stream"], component: stub("Streams") },
  { path: "/train/stream", title: "Topic", section: "train", workspace: true, component: stub("Topic") },
];
