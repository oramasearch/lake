/** Ship → Search Index (web_ui.cpp index_content): what one index covers, publish and unpublish,
 * a search box whose question stays in the URL (`q`, `field`, `top_k`), and how to call it from
 * outside with a key that can only search. */
import { useState } from "react";
import { Link, useNavigate, useSearchParams } from "react-router-dom";
import { act, type Row } from "@/lib/api";
import { useSession } from "@/lib/session";
import { entityCrumb, useTrail } from "@/lib/trail";
import { Badge, Button, CopyBlock, DataTable, ErrorBlock, Field, Query, Skeleton } from "@/lib/ui";
import { WorkspaceFrame, cellText, qs, text, useAsync, useWorkspace, useWq, useWsPath, wa } from "../train/workspace";

const SUB = "Search it here, publish it, and call it from your own code with a key that can only search.";

export default function IndexPage() {
  const [params] = useSearchParams();
  const id = params.get("id") ?? "";
  const { customerId } = useWorkspace();
  const ws = useWsPath();
  const space = qs({ customer_id: customerId });
  useTrail([
    { label: "Ship", to: ws(`/ship/models?${space}`) },
    { label: "Search Indexes", to: ws(`/ship/indexes?${space}`) },
    entityCrumb(id || "Search Index"),
  ]);
  return (
    <WorkspaceFrame eyebrow="Ship" title={id ? `Search Index: ${id}` : "Search Index"} sub={SUB}>
      {() => <Index id={id} />}
    </WorkspaceFrame>
  );
}

function Index({ id }: { id: string }) {
  const index = useWq<Row>("get_index", { index_id: id }, { enabled: !!id });
  if (!id) return <div className="panel empty">This index could not be read from the workspace.</div>;
  return (
    <Query q={index} what="the index" isEmpty={(v) => !v || Object.keys(v).length === 0} empty={<div className="panel empty">This index could not be read from the workspace.</div>}>
      {(value) => <IndexContent id={id} index={value} />}
    </Query>
  );
}

function IndexContent({ id, index }: { id: string; index: Row }) {
  const scope = useWorkspace();
  const published = index.exposed === true;
  const fields: string[] = Array.isArray(index.fields) ? index.fields.map(cellText) : [];
  const publish = useAsync();
  return (
    <>
      <section className="panel">
        <dl>
          <dt>Table</dt>
          <dd>{text(index, "table")}</dd>
          <dt>Kind</dt>
          <dd>{text(index, "kind")}</dd>
          <dt>Backend</dt>
          <dd>{text(index, "backend")}</dd>
          <dt>Fields</dt>
          <dd>{fields.join(", ")}</dd>
          <dt>Created</dt>
          <dd>{text(index, "created")}</dd>
          <dt>Published</dt>
          <dd>{published ? <Badge tone="success">published</Badge> : <Badge>not published</Badge>}</dd>
        </dl>
        {publish.error ? <ErrorBlock error={publish.error} /> : null}
        <Button
          variant={published ? "secondary" : "primary"}
          loading={publish.busy}
          onClick={() => void publish.run(() => wa(scope, published ? "unexpose_index" : "expose_index", { index: id }))}
        >
          {published ? "Unpublish" : "Publish"}
        </Button>
      </section>
      <TrySearch id={id} fields={fields} />
      <CallIt id={id} table={text(index, "table")} published={published} />
    </>
  );
}

function TrySearch({ id, fields }: { id: string; fields: string[] }) {
  const [params] = useSearchParams();
  const navigate = useNavigate();
  const ws = useWsPath();
  const { customerId } = useWorkspace();
  const asked = params.get("q") ?? "";
  const field = params.get("field") ?? "";
  const rawTopK = params.get("top_k") ?? "";
  const topK = rawTopK ? Math.min(100, Math.max(1, Number.parseInt(rawTopK, 10) || 0)) : 10;
  const [q, setQ] = useState(asked);
  const [f, setF] = useState(field);
  const [k, setK] = useState(String(topK));
  const search = useWq<{ hits?: Row[]; duration_ms?: number }>(
    "search",
    { target: id, query: asked, ...(field ? { field } : {}), top_k: topK },
    { enabled: !!asked },
  );
  return (
    <section className="panel">
      <h2>Try a Search</h2>
      <form
        className="stack"
        onSubmit={(e) => {
          e.preventDefault();
          navigate(ws(`/ship/index?${qs({ customer_id: customerId, id, q, field: f, top_k: k })}`));
        }}
      >
        <Field label="Search for" required>
          <input name="q" value={q} onChange={(e) => setQ(e.target.value)} placeholder="renewal risk" />
        </Field>
        <Field label="In">
          <select name="field" value={f} onChange={(e) => setF(e.target.value)}>
            <option value="">all fields</option>
            {fields.map((name) => (
              <option key={name}>{name}</option>
            ))}
          </select>
        </Field>
        <Field label="Hits">
          <input name="top_k" type="number" min={1} max={100} value={k} onChange={(e) => setK(e.target.value)} />
        </Field>
        <div>
          <Button type="submit" variant="primary">
            Search
          </Button>
        </div>
      </form>
      {asked &&
        (search.isLoading ? (
          <Skeleton />
        ) : search.isError ? (
          <ErrorBlock error={search.error} retry={() => void search.refetch()} what="the search" />
        ) : (
          <>
            <p className="muted">
              {(search.data?.hits ?? []).length} hits in {cellText(search.data?.duration_ms ?? 0)} ms.
            </p>
            {(search.data?.hits ?? []).length === 0 ? (
              <p>Nothing matched.</p>
            ) : (
              <DataTable
                rows={(search.data?.hits ?? []).map((hit, i): Row => ({ ...hit, rank: i + 1 }))}
                caption="Search hits"
                columns={[
                  { key: "rank", label: "#", render: (row) => String(row.rank) },
                  { key: "id", label: "Document", render: (row) => <code>{text(row, "id")}</code> },
                  { key: "score", label: "Score", render: (row) => cellText(row.score ?? 0) },
                ]}
              />
            )}
          </>
        ))}
    </section>
  );
}

/** The origin outside callers use: lake's configured external origin, its public host, else this
 * page's origin (the server page fell back to lake.oramadev.net). */
function useOrigin(): string {
  const { viewer } = useSession();
  if (viewer.external_origin) return viewer.external_origin;
  if (viewer.public_host) return `https://${viewer.public_host}`;
  return window.location.origin;
}

function CallIt({ id, table, published }: { id: string; table: string; published: boolean }) {
  const { customerId, workspaceId } = useWorkspace();
  const origin = useOrigin();
  const create = useAsync();
  const [secret, setSecret] = useState<string | null>(null);
  if (!published) {
    return (
      <section className="panel">
        <h2>Call It from Your Code</h2>
        <p>Publish the index first: only published indexes answer outside calls.</p>
      </section>
    );
  }
  const workspace = workspaceId || "<workspace id>";
  const url = `${origin}/v1/search_index?customer_id=${encodeURIComponent(customerId)}&workspace_id=${encodeURIComponent(workspace)}&index_id=${encodeURIComponent(id)}&query=`;
  const curl = `curl -s -H "Authorization: Bearer $LAKE_SEARCH_KEY" \\\n  "${url}renewal%20risk&top_k=10"`;
  const js = `// JavaScript\nconst res = await fetch("${url}" + encodeURIComponent("renewal risk"), {\n  headers: { Authorization: \`Bearer \${process.env.LAKE_SEARCH_KEY}\` },\n});\nconst { value } = await res.json(); // value.hits: [{ id, score }]`;
  const py = `# Python\nimport os, requests\nr = requests.get("${origin}/v1/search_index", params={"customer_id": "${customerId}", "workspace_id": "${workspace}", "index_id": "${id}", "query": "renewal risk"},\n    headers={"Authorization": f"Bearer {os.environ['LAKE_SEARCH_KEY']}"})\nhits = r.json()["value"]["hits"]`;
  return (
    <section className="panel">
      <h2>Call It from Your Code</h2>
      <ol>
        <li>
          <p>Create a key that can only search published indexes, and copy it; it is shown once.</p>
          {create.error ? <ErrorBlock error={create.error} /> : null}
          {secret ? (
            <CopyBlock value={secret} label="Your search key" />
          ) : (
            <Button
              loading={create.busy}
              onClick={() =>
                void create.run(async () => {
                  const issued = await act<{ secret?: string }>("create_token", { name: `search ${table}`, scopes: ["search_index"] });
                  setSecret(issued.secret ?? "");
                })
              }
            >
              Create a search key
            </Button>
          )}
        </li>
        <li>
          <p>Call lake with it. The search runs in your own workspace; lake checks the key and records every query.</p>
          <pre>
            <code>{curl}</code>
          </pre>
          <pre>
            <code>{js}</code>
          </pre>
          <pre>
            <code>{py}</code>
          </pre>
        </li>
        <li>
          <p>
            From an AI agent, the same search is the MCP tool <code>search_index</code> at <code>{origin}/mcp</code>.
          </p>
        </li>
      </ol>
      <p className="muted">
        Unpublishing the index stops every outside call at once; the key keeps working for other published indexes until you revoke it on{" "}
        <Link to="/tokens">Tokens</Link>. A key lasts as long as the session that made it, at most 30 days.
      </p>
    </section>
  );
}
