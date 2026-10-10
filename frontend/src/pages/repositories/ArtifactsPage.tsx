/** /artifacts?repository_id=&action=&output=&content=&cursor=&limit= (web_ui_artifacts): build
 * results, filtered and paged by the capability (limit 1–1000, default 100). */
import { useState } from "react";
import { Link, useSearchParams } from "react-router-dom";
import { DataTable } from "@/lib/ui";
import { dateTime } from "@/lib/format";
import { ForgeBadge, ForgePage, itemsOf, pending, repositoryName, ToRepositories, useForgeRead, useRepositoryTrail, type Artifact, type Repository } from "./forge";

type Page = { items: Artifact[]; next_cursor: string; done: boolean };

/** `query_page_limit`: 1–1000, otherwise 100. */
function pageLimit(value: string | null): number {
  const n = Number(value);
  return Number.isInteger(n) && n >= 1 && n <= 1000 ? n : 100;
}

export default function ArtifactsPage() {
  const [params] = useSearchParams();
  const id = params.get("repository_id") ?? "";
  if (!id) return <ToRepositories />;
  return <Artifacts id={id} />;
}

function Artifacts({ id }: { id: string }) {
  const [params, setParams] = useSearchParams();
  const filter = {
    action: params.get("action") ?? "",
    output: params.get("output") ?? "",
    content: params.get("content") ?? "",
    limit: pageLimit(params.get("limit")),
  };
  const cursor = params.get("cursor") ?? "";
  const [draft, setDraft] = useState({ ...filter, limit: String(filter.limit) });
  const repository = useForgeRead<Repository>("get_repository", { id });
  const page = useForgeRead<Page>("list_artifacts", { id, action: filter.action, output: filter.output, content: filter.content, cursor, limit: filter.limit });
  useRepositoryTrail(repository.data, "Artifacts");
  const wait = pending(repository);
  if (wait) return wait;
  const encoded = encodeURIComponent(id);
  const next = new URLSearchParams({ repository_id: id, cursor: page.data?.next_cursor ?? "", limit: String(filter.limit) });
  if (filter.action) next.set("action", filter.action);
  if (filter.output) next.set("output", filter.output);
  if (filter.content) next.set("content", filter.content);
  return (
    <ForgePage eyebrow="repository artifacts" title="Build Results" sub={repositoryName(repository.data!)}>
      <form
        className="filters"
        aria-label="Filter build results"
        onSubmit={(e) => {
          e.preventDefault();
          const search = new URLSearchParams({ repository_id: id, action: draft.action, output: draft.output, content: draft.content, limit: draft.limit });
          setParams(search);
        }}
      >
        <label>
          Action
          <input value={draft.action} onChange={(e) => setDraft({ ...draft, action: e.target.value })} />
        </label>
        <label>
          Output path
          <input value={draft.output} onChange={(e) => setDraft({ ...draft, output: e.target.value })} />
        </label>
        <label>
          Content digest
          <input value={draft.content} onChange={(e) => setDraft({ ...draft, content: e.target.value })} />
        </label>
        <label>
          Page size
          <input type="number" min={1} max={1000} value={draft.limit} onChange={(e) => setDraft({ ...draft, limit: e.target.value })} />
        </label>
        <button type="submit">Filter</button>
      </form>
      <DataTable<Artifact>
        q={page}
        what="build results"
        caption="Build results"
        rows={itemsOf(page.data)}
        rowKey={(artifact) => artifact.id}
        empty={<div className="panel empty">No build results match these filters.</div>}
        columns={[
          { key: "result", label: "Result", render: (a) => <Link to={`/artifact?repository_id=${encoded}&id=${encodeURIComponent(a.id)}`}>{a.id}</Link> },
          { key: "action", label: "Action", render: (a) => <code>{a.action}</code> },
          { key: "outputs", label: "Outputs", render: (a) => a.outputs.map((output) => output.path).join(", ") },
          { key: "cache", label: "Cache", render: (a) => <ForgeBadge value={a.divergent ? "divergent" : a.cache_status} /> },
          { key: "observed", label: "Observed", render: (a) => dateTime(a.observed_at) },
        ]}
      />
      {page.data && !page.data.done && page.data.next_cursor && (
        <p>
          <Link to={`/artifacts?${next.toString()}`}>Next page</Link>
        </p>
      )}
      <p>
        <Link to={`/repository?id=${encoded}`}>Back to repository</Link>
      </p>
    </ForgePage>
  );
}
