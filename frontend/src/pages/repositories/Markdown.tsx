/** The forge renders README and issue Markdown as an article. */
import { Markdown as Base, markdownAnchor } from "@/lib/markdown";

export { markdownAnchor };

export function Markdown({ children }: { children: string }) {
  return <Base as="article">{children}</Base>;
}
