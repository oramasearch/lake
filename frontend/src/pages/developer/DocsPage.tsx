/** Docs (`/docs`, `/docs/:page`, lake#561), after rev-os's Docs reader: lake's guides
 * (docs/guide/*.md) from `list_docs` and `get_doc`. A left navigation grouped by section, a search
 * (titles and headings at once, the whole text through `list_docs {q}`), the guide with a "#" link
 * on every heading, previous and next, and "On this page" with the heading in view marked. A
 * `#anchor` in the address opens at that heading. On a phone the navigation folds behind Contents
 * and "On this page" sits above the guide. */
import { type ReactNode, useEffect, useMemo, useRef, useState } from "react";
import { Link, useLocation, useParams } from "react-router-dom";
import { ArrowLeft, ArrowRight, ArrowUpRight, BookOpen, Check, ChevronRight, Link2, ListTree, Search, X } from "lucide-react";
import { ApiError, useRead } from "@/lib/api";
import { useTrail } from "@/lib/trail";
import { Button, Empty, ErrorBlock, Skeleton, cx } from "@/lib/ui";
import { Markdown } from "./Markdown";
import { type Doc, type DocHeading, type DocPage, type DocSection, docHref, filterDocs, groupBySection, splitAround, tableOfContents } from "./docs";

const NO_WORKSPACE = { workspace: null } as const;
type Contents = { pages: DocPage[]; sections?: string[] };

export default function DocsPage() {
  const { page } = useParams();
  const location = useLocation();
  const contents = useRead<Contents>("list_docs", undefined, { ...NO_WORKSPACE, staleTime: 300_000 });
  const pages = useMemo(() => contents.data?.pages ?? [], [contents.data]);
  const sections = useMemo(() => groupBySection(pages, contents.data?.sections), [pages, contents.data]);
  const first = pages[0]?.slug;
  // `/docs` is the first guide.
  const slug = page ?? first;
  const current = pages.find((p) => p.slug === slug);
  const guide = useRead<Doc>(slug ? "get_doc" : null, { slug }, { ...NO_WORKSPACE, staleTime: 300_000 });
  useTrail([{ label: "Docs", to: "/docs" }, ...(guide.data ? [{ label: guide.data.title }] : [])]);

  const [q, setQ] = useState("");
  const searching = q.trim().length > 0;
  // Phones and tablets: the navigation folds behind Contents so the guide comes first.
  const [navOpen, setNavOpen] = useState(false);
  useEffect(() => {
    setNavOpen(false);
    setQ("");
  }, [slug]);

  return (
    <div className="page-container docs-reader">
      <div className="grid grid-cols-1 gap-4 lg:grid-cols-[280px_minmax(0,1fr)] lg:gap-8">
        <aside className="docs-nav lg:sticky lg:top-4 lg:max-h-[calc(100vh-6rem)] lg:self-start lg:overflow-y-auto lg:pr-1">
          <div className="mb-3 flex items-center gap-2">
            <BookOpen size={15} className="text-action" aria-hidden />
            <Link to="/docs" className="text-[14px] font-semibold text-ink no-underline">
              Docs
            </Link>
            <button type="button" className="button ml-auto lg:hidden" aria-expanded={navOpen} aria-controls="docs-sections" onClick={() => setNavOpen((open) => !open)}>
              <ListTree size={14} aria-hidden /> Contents
            </button>
          </div>
          <div className="mb-3">
            <DocsSearchBox value={q} onChange={setQ} />
          </div>
          <div id="docs-sections" className={cx(navOpen ? "block" : "hidden", "lg:block")}>
            {contents.isLoading ? (
              <Skeleton lines={8} />
            ) : contents.isError ? (
              <ErrorBlock error={contents.error} retry={() => void contents.refetch()} what="the docs" />
            ) : (
              <SectionNav sections={sections} slug={slug} first={first} headings={current?.headings ?? []} />
            )}
            <div className="mt-4 space-y-1 border-t border-hairline pt-3 text-[12px]">
              <a href="/llms.txt" className="flex items-center gap-1 text-ink-3 no-underline hover:text-ink">
                llms.txt <ArrowUpRight size={11} aria-hidden />
              </a>
              <Link to="/developer/api" className="flex items-center gap-1 text-ink-3 no-underline hover:text-ink">
                API reference <ArrowRight size={11} aria-hidden />
              </Link>
              <Link to="/developer/mcp" className="flex items-center gap-1 text-ink-3 no-underline hover:text-ink">
                MCP setup <ArrowRight size={11} aria-hidden />
              </Link>
            </div>
          </div>
        </aside>

        <div className="min-w-0">
          {searching ? (
            <SearchPanel q={q} pages={pages} first={first} onPick={() => setQ("")} />
          ) : !slug ? (
            contents.isLoading ? <Skeleton lines={8} /> : contents.isError ? null : <Empty icon={BookOpen} title="No Guides">No guides are included in this Lake.</Empty>
          ) : guide.isLoading ? (
            <Skeleton lines={14} />
          ) : guide.isError ? (
            guide.error instanceof ApiError && guide.error.status === 404 ? (
              <Empty icon={BookOpen} title="No Such Page" action={<Button to="/docs">All docs</Button>}>
                There is no Docs page named “{slug}”.
              </Empty>
            ) : (
              <ErrorBlock error={guide.error} retry={() => void guide.refetch()} what="this guide" />
            )
          ) : guide.data ? (
            <Article doc={guide.data} headings={current?.headings ?? []} first={first} hash={location.hash} />
          ) : null}
        </div>
      </div>
    </div>
  );
}

/** The sections, each a label over its pages; the open page lists its sections beneath it. */
function SectionNav({ sections, slug, first, headings }: { sections: DocSection[]; slug?: string; first?: string; headings: DocHeading[] }) {
  const toc = headings.filter((h) => h.level === 2);
  return (
    <nav aria-label="Docs" className="space-y-4">
      {sections.map((section) => (
        <div key={section.name}>
          <div className="label mb-1 px-2.5">{section.name}</div>
          <ul className="m-0 list-none space-y-px p-0">
            {section.pages.map((p) => {
              const on = p.slug === slug;
              return (
                <li key={p.slug}>
                  <Link
                    to={docHref(p.slug, first)}
                    aria-current={on ? "page" : undefined}
                    className={cx(
                      "docs-nav-link block rounded-[5px] px-2.5 py-[9px] text-[13px] leading-snug no-underline lg:py-[5px]",
                      on ? "bg-layer font-semibold text-ink" : "text-ink-2 hover:bg-layer hover:text-ink",
                    )}
                  >
                    {p.title}
                  </Link>
                  {on && toc.length > 0 && (
                    <ul className="m-0 mt-0.5 mb-1.5 ml-[14px] list-none border-l border-hairline p-0 xl:hidden">
                      {toc.map((h) => (
                        <li key={h.anchor}>
                          <a
                            href={`#${h.anchor}`}
                            onClick={(event) => {
                              event.preventDefault();
                              goTo(h.anchor);
                            }}
                            className="-ml-px block border-l border-transparent py-[7px] pr-2 pl-3 text-[12px] leading-snug text-ink-3 no-underline hover:border-rule hover:text-ink lg:py-[3px]"
                          >
                            {h.text}
                          </a>
                        </li>
                      ))}
                    </ul>
                  )}
                </li>
              );
            })}
          </ul>
        </div>
      ))}
    </nav>
  );
}

/** Scroll to a heading and put its anchor in the address, without a navigation. */
function goTo(anchor: string) {
  document.getElementById(anchor)?.scrollIntoView({ behavior: "smooth", block: "start" });
  history.replaceState(history.state, "", `#${anchor}`);
}

function DocsSearchBox({ value, onChange }: { value: string; onChange: (v: string) => void }) {
  const ref = useRef<HTMLInputElement>(null);
  return (
    <div className="docs-search flex h-10 items-center gap-2 rounded-[7px] border border-rule bg-surface px-2.5 lg:h-9">
      <Search size={14} className="shrink-0 text-ink-3" aria-hidden />
      <input
        ref={ref}
        type="search"
        value={value}
        onChange={(e) => onChange(e.target.value)}
        onKeyDown={(e) => {
          if (e.key === "Escape") {
            onChange("");
            ref.current?.blur();
          }
        }}
        placeholder="Search the docs"
        aria-label="Search the docs"
        className="plain min-w-0 flex-1 border-0 bg-transparent text-[13px] text-ink placeholder:text-ink-3 focus:outline-none"
      />
      {value && (
        <button type="button" onClick={() => onChange("")} className="plain hit rounded p-0.5 text-ink-3 hover:text-ink" aria-label="Clear search">
          <X size={13} aria-hidden />
        </button>
      )}
    </div>
  );
}

/** Titles and headings at once from the contents already read; the whole text a moment later from
 * `list_docs {q}`, each with the heading to open and an excerpt. */
function SearchPanel({ q, pages, first, onPick }: { q: string; pages: DocPage[]; first?: string; onPick: () => void }) {
  const query = q.trim();
  const [debounced, setDebounced] = useState(query);
  useEffect(() => {
    const t = setTimeout(() => setDebounced(query), 200);
    return () => clearTimeout(t);
  }, [query]);
  const instant = useMemo(() => filterDocs(pages, query), [pages, query]);
  const full = useRead<{ pages: DocPage[] }>(debounced.length >= 2 ? "list_docs" : null, { q: debounced }, { ...NO_WORKSPACE, staleTime: 300_000 });
  const instantKeys = new Set(instant.map((hit) => `${hit.slug}#${hit.heading?.anchor ?? ""}`));
  const more = (full.data?.pages ?? []).filter((p) => p.match === "text" && !instantKeys.has(`${p.slug}#${p.anchor ?? ""}`));

  return (
    <div>
      <div className="label mb-3">Search · “{query}”</div>
      {instant.length > 0 && (
        <ResultList label="Pages and sections">
          {instant.map((hit) => (
            <ResultRow
              key={`${hit.slug}#${hit.heading?.anchor ?? ""}`}
              to={docHref(hit.slug, first, hit.heading?.anchor)}
              section={hit.section}
              title={hit.heading ? hit.heading.text : hit.title}
              detail={hit.heading ? hit.title : undefined}
              q={query}
              onPick={onPick}
            />
          ))}
        </ResultList>
      )}
      {debounced.length < 2 ? (
        instant.length === 0 && <p className="text-[13px] text-ink-3">Type at least two letters to search the text.</p>
      ) : full.isLoading ? (
        <p className="text-[13px] text-ink-3">Searching the text…</p>
      ) : full.isError ? (
        <ErrorBlock error={full.error} retry={() => void full.refetch()} what="the search" />
      ) : more.length > 0 ? (
        <ResultList label="In the text">
          {more.map((p) => (
            <ResultRow key={`${p.slug}#${p.anchor ?? ""}`} to={docHref(p.slug, first, p.anchor)} section={p.section} title={p.title} detail={p.excerpt} q={query} onPick={onPick} />
          ))}
        </ResultList>
      ) : (
        instant.length === 0 && (
          <Empty icon={Search} title="Nothing Found" compact>
            No page mentions “{query}”. Try the name of a page or a button.
          </Empty>
        )
      )}
    </div>
  );
}

function ResultList({ label, children }: { label: string; children: ReactNode }) {
  return (
    <section className="mb-5" aria-label={label}>
      <div className="label mb-2">{label}</div>
      <div className="divide-y divide-hairline overflow-hidden rounded-[6px] border border-hairline bg-surface">{children}</div>
    </section>
  );
}

function ResultRow({ to, section, title, detail, q, onPick }: { to: string; section: string; title: string; detail?: string; q: string; onPick: () => void }) {
  return (
    <Link to={to} onClick={onPick} className="group block px-4 py-3 no-underline hover:bg-layer">
      <div className="font-mono text-[10px] tracking-[0.1em] text-ink-3 uppercase">{section}</div>
      <div className="mt-0.5 text-[14px] font-semibold text-ink">{highlight(title, q)}</div>
      {detail && <div className="mt-0.5 text-[12.5px] leading-relaxed text-ink-2">{highlight(detail, q)}</div>}
    </Link>
  );
}

function highlight(text: string, q: string) {
  const parts = splitAround(text, q);
  if (!parts) return text;
  return (
    <>
      {parts[0]}
      <mark className="docs-mark">{parts[1]}</mark>
      {parts[2]}
    </>
  );
}

function Article({ doc, headings, first, hash }: { doc: Doc; headings: DocHeading[]; first?: string; hash: string }) {
  const [copied, setCopied] = useState(false);
  const toc = tableOfContents(headings);

  // Deep links: open at the #anchor once the guide is in, otherwise at the top.
  useEffect(() => {
    const id = decodeURIComponent(hash.replace(/^#/, ""));
    const t = setTimeout(() => {
      if (id) document.getElementById(id)?.scrollIntoView({ block: "start" });
      else window.scrollTo({ top: 0 });
    }, 30);
    return () => clearTimeout(t);
  }, [doc.slug, hash]);

  const copy = () => {
    void navigator.clipboard?.writeText(window.location.origin + docHref(doc.slug, first)).then(() => {
      setCopied(true);
      setTimeout(() => setCopied(false), 1400);
    });
  };

  return (
    <div className="grid grid-cols-1 gap-10 xl:grid-cols-[minmax(0,1fr)_220px]">
      <article className="min-w-0">
        <nav aria-label="Breadcrumb" className="mb-3 flex flex-wrap items-center gap-1.5 text-[12px] text-ink-3">
          <Link to="/docs" className="text-ink-3 no-underline hover:text-ink">
            Docs
          </Link>
          <ChevronRight size={11} aria-hidden />
          <span>{doc.section}</span>
          <ChevronRight size={11} aria-hidden />
          <span className="text-ink-2">{doc.title}</span>
          <Button className="ml-auto" onClick={copy}>
            {copied ? <Check size={13} aria-hidden /> : <Link2 size={13} aria-hidden />} {copied ? "Link copied" : "Copy link"}
          </Button>
        </nav>
        {toc.length > 0 && (
          <details className="docs-toc-inline mb-4 rounded-[6px] border border-hairline bg-surface px-3 py-2 xl:hidden">
            <summary className="cursor-pointer text-[13px] font-semibold text-ink">On this page</summary>
            <TocList headings={toc} />
          </details>
        )}
        <div className="docs-page">
          <Markdown headingLinks>{doc.markdown}</Markdown>
        </div>
        <nav aria-label="Previous and next" className="mt-10 grid grid-cols-1 gap-3 border-t border-hairline pt-5 sm:grid-cols-2">
          {doc.previous ? (
            <Link to={docHref(doc.previous.slug, first)} className="docs-step group rounded-[7px] border border-hairline bg-surface px-4 py-3 no-underline hover:border-rule">
              <div className="flex items-center gap-1 font-mono text-[10px] tracking-[0.1em] text-ink-3 uppercase">
                <ArrowLeft size={11} aria-hidden /> Previous
              </div>
              <div className="mt-0.5 text-[13.5px] font-semibold text-ink">{doc.previous.title}</div>
            </Link>
          ) : (
            <span />
          )}
          {doc.next && (
            <Link to={docHref(doc.next.slug, first)} className="docs-step group rounded-[7px] border border-hairline bg-surface px-4 py-3 text-right no-underline hover:border-rule">
              <div className="flex items-center justify-end gap-1 font-mono text-[10px] tracking-[0.1em] text-ink-3 uppercase">
                Next <ArrowRight size={11} aria-hidden />
              </div>
              <div className="mt-0.5 text-[13.5px] font-semibold text-ink">{doc.next.title}</div>
            </Link>
          )}
        </nav>
      </article>
      {toc.length > 0 && (
        <aside className="hidden xl:block">
          <div className="sticky top-4">
            <div className="label mb-2">On this page</div>
            <TocList headings={toc} spy />
          </div>
        </aside>
      )}
    </div>
  );
}

/** The page's headings; with `spy`, the one in view is marked as the reader scrolls. */
function TocList({ headings, spy }: { headings: DocHeading[]; spy?: boolean }) {
  const [active, setActive] = useState<string | null>(headings[0]?.anchor ?? null);
  useEffect(() => {
    if (!spy || typeof IntersectionObserver === "undefined") return;
    const elements = headings.map((h) => document.getElementById(h.anchor)).filter((e): e is HTMLElement => !!e);
    if (!elements.length) return;
    const observer = new IntersectionObserver(
      (entries) => {
        const visible = entries.filter((e) => e.isIntersecting).sort((a, b) => a.boundingClientRect.top - b.boundingClientRect.top);
        if (visible[0]) setActive(visible[0].target.id);
      },
      { rootMargin: "-72px 0px -65% 0px", threshold: [0, 1] },
    );
    elements.forEach((element) => observer.observe(element));
    return () => observer.disconnect();
  }, [headings, spy]);
  return (
    <ul className="m-0 mt-1 list-none space-y-px border-l border-hairline p-0">
      {headings.map((h) => (
        <li key={h.anchor}>
          <a
            href={`#${h.anchor}`}
            onClick={(event) => {
              event.preventDefault();
              goTo(h.anchor);
              setActive(h.anchor);
            }}
            className={cx(
              "-ml-px block border-l py-[6px] text-[12.5px] leading-snug no-underline xl:py-[2px]",
              h.level === 3 ? "pl-5" : "pl-3",
              spy && active === h.anchor ? "border-action text-action" : "border-transparent text-ink-3 hover:text-ink",
            )}
          >
            {h.text}
          </a>
        </li>
      ))}
    </ul>
  );
}
