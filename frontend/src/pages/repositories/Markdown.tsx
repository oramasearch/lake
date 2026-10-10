/** Markdown as lake renders it (lake/api/markdown.hpp): `<article class="markdown-body">`, raw HTML
 * left as text, and every heading given a stable lowercase anchor, a repeated heading numbered
 * -2, -3, … (docs/ui/forge.md). Rendered in the browser with react-markdown + remark-gfm. */
import { Children, isValidElement, type ReactNode } from "react";
import ReactMarkdown, { type Components } from "react-markdown";
import remarkGfm from "remark-gfm";

/** The text of a heading's children. */
function textOf(node: ReactNode): string {
  if (node === null || node === undefined || typeof node === "boolean") return "";
  if (typeof node === "string" || typeof node === "number") return String(node);
  if (Array.isArray(node)) return node.map(textOf).join("");
  if (isValidElement<{ children?: ReactNode }>(node)) return textOf(node.props.children);
  return Children.toArray(node).map(textOf).join("");
}

/** lake's `markdown_anchor`: letters and digits lowercased, a space or hyphen run as one dash. */
export function markdownAnchor(heading: string): string {
  let out = "";
  let dash = false;
  for (const c of heading) {
    if (/^[A-Za-z0-9]$/.test(c)) {
      if (dash && out) out += "-";
      out += c.toLowerCase();
      dash = false;
    } else if (c === " " || c === "-") {
      dash = true;
    }
  }
  return out;
}

export function Markdown({ children }: { children: string }) {
  // One counter per rendered document, so a repeated heading reads "-2", "-3".
  const seen = new Map<string, number>();
  const anchor = (content: ReactNode) => {
    const base = markdownAnchor(textOf(content));
    const count = (seen.get(base) ?? 0) + 1;
    seen.set(base, count);
    return count > 1 ? `${base}-${count}` : base;
  };
  const components: Components = {
    h1: ({ children: c }) => <h1 id={anchor(c)}>{c}</h1>,
    h2: ({ children: c }) => <h2 id={anchor(c)}>{c}</h2>,
    h3: ({ children: c }) => <h3 id={anchor(c)}>{c}</h3>,
    h4: ({ children: c }) => <h4 id={anchor(c)}>{c}</h4>,
    h5: ({ children: c }) => <h5 id={anchor(c)}>{c}</h5>,
    h6: ({ children: c }) => <h6 id={anchor(c)}>{c}</h6>,
  };
  return (
    <article className="markdown-body">
      <ReactMarkdown remarkPlugins={[remarkGfm]} components={components}>
        {children}
      </ReactMarkdown>
    </article>
  );
}
