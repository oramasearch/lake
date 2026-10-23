/** Markdown as lake renders it (lake/api/markdown.hpp), in the browser with react-markdown and
 * remark-gfm: raw HTML stays text, every heading gets a stable lowercase anchor (a repeated one
 * numbered -2, -3, …; docs/ui/forge.md), links to app pages stay in the app and every other link
 * is a plain link, so server routes such as /llms.txt load from the server. Unsafe link schemes are
 * dropped by react-markdown's URL filter. An image is never loaded, only named (markdownImages.ts,
 * lake#943). */
import { Children, isValidElement, type ReactNode } from "react";
import ReactMarkdown, { type Components } from "react-markdown";
import remarkGfm from "remark-gfm";
import { Link } from "react-router-dom";
import { isServerRoute } from "@/routes";
import { inertImages } from "./markdownImages";

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

/** A same-origin path to a page of the app (not a server route or an anchor). */
function appPath(href: string | undefined): boolean {
  return !!href && href.startsWith("/") && !href.startsWith("//") && !isServerRoute(href.split(/[?#]/)[0]);
}

/** `headingLinks`: every heading below the title carries a "#" link to itself, so a reader can copy
 * the address of a section (the Docs, lake#561). */
export function Markdown({ children, inline, as = "div", headingLinks }: { children: string; inline?: boolean; as?: "div" | "article"; headingLinks?: boolean }) {
  // One counter per rendered document, so a repeated heading reads "-2", "-3".
  const seen = new Map<string, number>();
  const anchor = (content: ReactNode) => {
    const base = markdownAnchor(textOf(content));
    const count = (seen.get(base) ?? 0) + 1;
    seen.set(base, count);
    return count > 1 ? `${base}-${count}` : base;
  };
  const linked = (Tag: "h2" | "h3" | "h4" | "h5" | "h6") =>
    function Heading({ children: c }: { children?: ReactNode }) {
      const id = anchor(c);
      return (
        <Tag id={id}>
          {c}
          {headingLinks && (
            <a className="heading-link" href={`#${id}`} aria-label={`Link to ${textOf(c)}`}>
              #
            </a>
          )}
        </Tag>
      );
    };
  const components: Components = {
    ...inertImages,
    h1: ({ children: c }) => <h1 id={anchor(c)}>{c}</h1>,
    h2: linked("h2"),
    h3: linked("h3"),
    h4: linked("h4"),
    h5: linked("h5"),
    h6: linked("h6"),
    a: ({ href, children: text }) => (appPath(href) ? <Link to={href!}>{text}</Link> : <a href={href}>{text}</a>),
  };
  const Box = as;
  return (
    <Box className={inline ? "markdown-body markdown-inline" : "markdown-body"}>
      <ReactMarkdown remarkPlugins={[remarkGfm]} components={components}>
        {children}
      </ReactMarkdown>
    </Box>
  );
}
