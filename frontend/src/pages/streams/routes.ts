import type { Screen } from "@/routes/types";
import { lazyPage } from "@/lib/lazyPage";

export const screens: Screen[] = [
  { path: "/train/streams", title: "Streams", section: "train", nav: true, icon: "streams", workspace: true, owns: ["/train/stream"], component: lazyPage(() => import("./StreamsPage")) },
  { path: "/train/stream", title: "Topic", section: "train", workspace: true, component: lazyPage(() => import("./TopicPage")) },
];
