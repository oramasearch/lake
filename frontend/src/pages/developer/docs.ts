// SPDX-License-Identifier: MIT

/** The Docs reader's rules (lake#561): the guides `list_docs` answers grouped by section, the
 * instant filter over titles and headings, and the addresses of a page and a heading. No imports
 * from the app, so `tests/docs.test.ts` runs it directly. */

export type DocHeading = { level: number; text: string; anchor: string };
export type DocRef = { slug: string; title: string; section?: string };
export type DocPage = DocRef & {
  section: string;
  headings: DocHeading[];
  /** Present only on a `list_docs {q}` answer. */
  match?: "title" | "heading" | "text";
  anchor?: string;
  excerpt?: string;
};
export type Doc = DocRef & { section: string; markdown: string; previous?: DocRef | null; next?: DocRef | null };
export type DocSection = { name: string; pages: DocPage[] };

/** The first guide is `/docs`; every other is `/docs/<slug>`, and a heading is `#<anchor>`. */
export function docHref(slug: string, first: string | undefined, anchor?: string | null): string {
  const base = slug === first ? "/docs" : `/docs/${encodeURIComponent(slug)}`;
  return anchor ? `${base}#${anchor}` : base;
}

/** The pages in `sections` order, each section once. A page whose section is missing from the list
 * (an older lake answers no `sections`) still gets a group, in reading order. */
export function groupBySection(pages: DocPage[], sections?: string[]): DocSection[] {
  const order = [...(sections ?? [])];
  for (const page of pages) {
    const name = page.section || "Docs";
    if (!order.includes(name)) order.push(name);
  }
  return order
    .map((name) => ({ name, pages: pages.filter((page) => (page.section || "Docs") === name) }))
    .filter((section) => section.pages.length > 0);
}

/** One instant result: a page whose title matches, or one of its headings. */
export type DocHit = { slug: string; title: string; section: string; heading?: DocHeading };

/** Titles first, then headings, in reading order, at most `limit`. Every word of the query must
 * appear (case ignored), so "branch protect" finds "Branch Protection". */
export function filterDocs(pages: DocPage[], q: string, limit = 12): DocHit[] {
  const words = q.toLowerCase().split(/\s+/).filter(Boolean);
  if (!words.length) return [];
  const all = (text: string) => {
    const lower = text.toLowerCase();
    return words.every((word) => lower.includes(word));
  };
  const titles: DocHit[] = [];
  const headings: DocHit[] = [];
  for (const page of pages) {
    const hit = { slug: page.slug, title: page.title, section: page.section };
    if (all(page.title)) titles.push(hit);
    for (const heading of page.headings ?? []) {
      if (all(heading.text)) headings.push({ ...hit, heading });
    }
  }
  return [...titles, ...headings].slice(0, limit);
}

/** The headings the "On this page" list shows: level 2, and level 3 under them. */
export function tableOfContents(headings: DocHeading[]): DocHeading[] {
  return headings.filter((heading) => heading.level === 2 || heading.level === 3);
}

/** `text` split around the first case-insensitive occurrence of `needle`, for highlighting. */
export function splitAround(text: string, needle: string): [string, string, string] | null {
  const n = needle.trim();
  if (!n) return null;
  const at = text.toLowerCase().indexOf(n.toLowerCase());
  if (at < 0) return null;
  return [text.slice(0, at), text.slice(at, at + n.length), text.slice(at + n.length)];
}
