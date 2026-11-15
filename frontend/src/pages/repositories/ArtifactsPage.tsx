// SPDX-License-Identifier: MIT

/** /artifacts?repository_id=&action=&output=&content=&cursor=&limit= (web_ui_artifacts): build
 * results, filtered and paged by the capability (limit 1–1000, default 100). */
import { useState } from "react";
import { Link, useSearchParams } from "react-router-dom";
import { Boxes, CircleX, Filter, GitFork, Zap } from "lucide-react";
import { Actions, Button, Card, DataTable, Empty, Stat, StatRow } from "@/lib/ui";
import { dateTime, relative } from "@/lib/format";
import { ForgePage, ForgeStatus, Identifier, itemsOf, pending, ToRepositories, useForgeRead, useRepositoryTrail, type Artifact, type Repository } from "./forge";

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
  const results = itemsOf(page.data);
  const hits = results.filter((a) => a.cache_status === "hit").length;
  const divergent = results.filter((a) => a.divergent).length;
  const failed = results.filter((a) => a.exit_code !== 0).length;
  const filtered = !!(filter.action || filter.output || filter.content);
  return (
    <ForgePage title="Build Results" repository={repository.data!} tab="artifacts" sub="Each build action's outputs, cache outcome and attestations.">
      {page.data && results.length > 0 && (
        <StatRow>
          <Stat label="On this page" value={results.length} icon={Boxes} sub={page.data.done ? "the last page" : "more on the next page"} />
          <Stat label="Cache hits" value={hits} icon={Zap} sub={`of ${results.length}`} />
          <Stat label="Failed" value={failed} icon={CircleX} tone={failed ? "bad" : undefined} sub="non-zero exit" />
          <Stat label="Divergent" value={divergent} icon={GitFork} tone={divergent ? "bad" : undefined} sub="outputs disagree" />
        </StatRow>
      )}
      <Card label="Filter build results" className="mb-4">
        <form
          className="filters m-0"
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
          <button type="submit">
            <Filter size={14} aria-hidden /> Filter
          </button>
        </form>
      </Card>
      <DataTable<Artifact>
        q={page}
        what="build results"
        caption="Build results"
        rows={results}
        rowKey={(artifact) => artifact.id}
        empty={
          <Empty icon={Boxes} title={filtered ? "No build results match these filters" : "No build results yet"}>
            {filtered ? "Change or clear a filter to see more." : "Results appear here when a build runs from this repository."}
          </Empty>
        }
        columns={[
          {
            key: "result",
            label: "Result",
            render: (a) => (
              <div className="min-w-0">
                <Link to={`/artifact?repository_id=${encoded}&id=${encodeURIComponent(a.id)}`}>
                  <Identifier value={a.id} />
                </Link>
                <div className="max-w-[28rem] truncate text-[12px] text-ink-3">{a.outputs.map((output) => output.path).join(", ") || "no outputs"}</div>
              </div>
            ),
          },
          { key: "action", label: "Action", sort: (a) => a.action, render: (a) => <code>{a.action}</code> },
          { key: "exit", label: "Exit", sort: (a) => a.exit_code, render: (a) => <ForgeStatus value={a.exit_code === 0 ? "passed" : "failed"} /> },
          { key: "cache", label: "Cache", render: (a) => <ForgeStatus value={a.divergent ? "divergent" : a.cache_status} /> },
          {
            key: "observed",
            label: "Observed",
            sort: (a) => a.observed_at,
            render: (a) => (
              <span className="font-mono text-[12px] text-ink-2" title={dateTime(a.observed_at)}>
                {relative(a.observed_at)}
              </span>
            ),
          },
        ]}
      />
      <Actions className="mt-3">
        {page.data && !page.data.done && page.data.next_cursor && <Button to={`/artifacts?${next.toString()}`}>Next page</Button>}
        <Button variant="ghost" to={`/repository?id=${encoded}`}>
          Back to repository
        </Button>
      </Actions>
    </ForgePage>
  );
}
