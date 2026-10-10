/** Docs (`/docs`, `/docs/:page`): lake's guides (docs/guide/*.md) from `list_docs` and `get_doc`,
 * the contents with the current guide marked, the guide in Markdown and a link to the next one. */
import { Link, useParams } from "react-router-dom";
import { useRead } from "@/lib/api";
import { useTrail } from "@/lib/trail";
import { ErrorBlock, Query, Skeleton } from "@/lib/ui";
import { Markdown } from "./Markdown";

type GuideRef = { slug: string; title: string };
type Guide = GuideRef & { markdown: string; next?: GuideRef | null };

const NO_WORKSPACE = { workspace: null } as const;

export default function DocsPage() {
  const { page } = useParams();
  const contents = useRead<{ pages: GuideRef[] }>("list_docs", undefined, { ...NO_WORKSPACE, staleTime: 300_000 });
  const pages = contents.data?.pages ?? [];
  const first = pages[0]?.slug;
  // `/docs` is the first guide.
  const slug = page ?? first;
  const guide = useRead<Guide>(slug ? "get_doc" : null, { page: slug }, { ...NO_WORKSPACE, staleTime: 300_000 });
  useTrail([{ label: "Docs", to: "/docs" }, ...(guide.data ? [{ label: guide.data.title }] : [])]);
  const href = (ref: GuideRef) => (ref.slug === first ? "/docs" : `/docs/${ref.slug}`);

  return (
    <div className="page-container">
      <p className="eyebrow">Docs</p>
      <div className="docs">
        <nav className="docs-contents" aria-label="Docs">
          <Query q={contents} what="the docs" lines={4}>
            {(value) => (
              <ul>
                {value.pages.map((ref) => (
                  <li key={ref.slug}>
                    <Link to={href(ref)} aria-current={ref.slug === slug ? "page" : undefined}>
                      {ref.title}
                    </Link>
                  </li>
                ))}
              </ul>
            )}
          </Query>
        </nav>
        <article className="docs-page">
          {!slug ? (
            contents.isLoading ? <Skeleton lines={8} /> : contents.isError ? null : <p className="muted">No guides are included in this lake.</p>
          ) : guide.isLoading ? (
            <Skeleton lines={8} />
          ) : guide.isError ? (
            <ErrorBlock error={guide.error} retry={() => void guide.refetch()} what="this guide" />
          ) : guide.data ? (
            <>
              <Markdown>{guide.data.markdown}</Markdown>
              {guide.data.next && (
                <p className="docs-next">
                  Next: <Link to={href(guide.data.next)}>{guide.data.next.title}</Link>
                </p>
              )}
            </>
          ) : null}
        </article>
      </div>
    </div>
  );
}
