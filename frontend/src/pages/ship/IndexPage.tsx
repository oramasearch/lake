/** Ship → Search Index (web_ui.cpp index_content): what one index covers, publish and unpublish,
 * a search playground whose question stays in the URL (`q`, `field`, `top_k`), and how to call it
 * from outside with a key that can only search. Laid out as a Split: the playground and the code
 * on the left; the facts, publishing and the search key on the right. */
import { useState } from "react";
import { Link, useNavigate, useSearchParams } from "react-router-dom";
import { Globe, KeyRound, Search, SearchX, Trash2 } from "lucide-react";
import { act, type Row } from "@/lib/api";
import { relative } from "@/lib/format";
import { ConfirmButton } from "@/lib/overlay";
import { indexLines } from "@/lib/deletion";
import { DeletionButton } from "@/lib/deletionDialog";
import { entityCrumb, useTrail } from "@/lib/trail";
import { Banner, Button, Card, Chip, CopyBlock, Empty, ErrorBlock, KeyValue, Meter, Query, Skeleton, Split, StateTabs, Status } from "@/lib/ui";
import { WorkspaceFrame, cellText, qs, text, useAsync, useWorkspace, useWq, useWsPath, wa } from "../train/workspace";
import { CopyMono, useOrigin } from "./parts";
import { hitFields } from "./search";

const SUB = "Search it here, publish it, and call it from your own code with a key that can only search.";

export default function IndexPage() {
  const [params] = useSearchParams();
  const id = params.get("id") ?? "";
  const navigate = useNavigate();
  const { customerId, workspaceId } = useWorkspace();
  const ws = useWsPath();
  const space = qs({ customer_id: customerId });
  // The same read the content makes (one cache entry): the header names the table and the state.
  const index = useWq<Row>("get_index", { index_id: id }, { enabled: !!id });
  const table = index.data ? text(index.data, "table") : "";
  useTrail([
    { label: "Ship", to: ws(`/ship/models?${space}`) },
    { label: "Search Indexes", to: ws(`/ship/indexes?${space}`) },
    entityCrumb(id || "Search Index"),
  ]);
  return (
    <WorkspaceFrame
      eyebrow="Ship"
      title={
        <span className="inline-flex flex-wrap items-center gap-2">
          {table ? `Index on ${table}` : "Search Index"}
          {index.data && <Status value={index.data.exposed === true ? "published" : "not published"} tone={index.data.exposed === true ? "success" : "neutral"} />}
        </span>
      }
      sub={SUB}
      actions={
        <>
          <Button to={ws(`/ship/indexes?${space}`)} variant="ghost">
            All indexes
          </Button>
          {index.data ? (
            <DeletionButton
              capability="delete_index"
              input={{ customer_id: customerId, workspace_id: workspaceId, id }}
              title={table ? `Delete the Index on ${table}` : "Delete Search Index"}
              confirmLabel="Delete index"
              intro="Deleting the index removes it from the workspace. It cannot be undone."
              typed={() => table || id.slice(0, 8)}
              lines={indexLines}
              done="Search index deleted"
              onDone={() => navigate(ws(`/ship/indexes?${space}`))}
            >
              <Trash2 size={14} aria-hidden /> Delete
            </DeletionButton>
          ) : null}
        </>
      }
    >
      {() => <Index id={id} />}
    </WorkspaceFrame>
  );
}

function Index({ id }: { id: string }) {
  const index = useWq<Row>("get_index", { index_id: id }, { enabled: !!id });
  const unread = (
    <Empty icon={SearchX} title="Index not found">
      This index could not be read from the workspace.
    </Empty>
  );
  if (!id) return unread;
  return (
    <Query q={index} what="the index" isEmpty={(v) => !v || Object.keys(v).length === 0} empty={unread}>
      {(value) => <IndexContent id={id} index={value} />}
    </Query>
  );
}

function IndexContent({ id, index }: { id: string; index: Row }) {
  const scope = useWorkspace();
  const published = index.exposed === true;
  const fields: string[] = Array.isArray(index.fields) ? index.fields.map(cellText) : [];
  const publish = useAsync();
  const toggle = () => publish.run(() => wa(scope, published ? "unexpose_index" : "expose_index", { index: id }));
  return (
    <Split
      side={
        <>
          <Card label="Index" title="Facts">
            <KeyValue
              rows={[
                ["Id", <CopyMono key="id" value={id} short={12} label="Copy the index id" />],
                ["Table", text(index, "table")],
                ["Kind", text(index, "kind") ? <Chip>{text(index, "kind")}</Chip> : null],
                ["Backend", text(index, "backend")],
                [
                  "Fields",
                  fields.length ? (
                    <span className="inline-flex flex-wrap gap-1">
                      {fields.map((f) => (
                        <Chip key={f}>{f}</Chip>
                      ))}
                    </span>
                  ) : null,
                ],
                ["Created", <span title={text(index, "created")}>{relative(text(index, "created"), Date.now(), text(index, "created"))}</span>],
              ]}
            />
          </Card>
          <Card
            label="Outside calls"
            title="Publishing"
            tone={published ? "active" : "default"}
            actions={<Status value={published ? "published" : "not published"} tone={published ? "success" : "neutral"} />}
          >
            <p className="mt-0 text-[13px] text-ink-2">
              {published ? "Answering outside calls with a search key." : "Only published indexes answer outside calls. Searching here works either way."}
            </p>
            {publish.error ? <ErrorBlock error={publish.error} /> : null}
            {published ? (
              <ConfirmButton
                title="Unpublish this index?"
                confirm="Unpublishing the index stops every outside call at once. Searching here keeps working, and you can publish it again later."
                confirmLabel="Unpublish"
                disabled={publish.busy}
                onConfirm={toggle}
              >
                Unpublish
              </ConfirmButton>
            ) : (
              <Button variant="primary" loading={publish.busy} onClick={() => void toggle()}>
                <Globe size={14} aria-hidden /> Publish
              </Button>
            )}
          </Card>
          <SearchKey table={text(index, "table")} published={published} />
        </>
      }
    >
      <TrySearch id={id} fields={fields} />
      <CallIt id={id} published={published} />
    </Split>
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
  const hits = search.data?.hits ?? [];
  const top = hits.reduce((m, hit) => Math.max(m, Number(hit.score ?? 0) || 0), 0);
  return (
    <Card
      label="Playground"
      title="Try a Search"
      actions={
        asked && search.isSuccess ? (
          <span className="font-mono text-[11.5px] text-ink-3 tabular-nums">
            {hits.length} hits in {cellText(search.data?.duration_ms ?? 0)} ms
          </span>
        ) : null
      }
    >
      <form
        className="flex flex-wrap items-end gap-2"
        role="search"
        onSubmit={(e) => {
          e.preventDefault();
          navigate(ws(`/ship/index?${qs({ customer_id: customerId, id, q, field: f, top_k: k })}`));
        }}
      >
        <label className="min-w-[12rem] flex-[3_1_14rem]">
          <span className="label m-0 block">Search for</span>
          <input name="q" required aria-required className="w-full" value={q} onChange={(e) => setQ(e.target.value)} placeholder="renewal risk" />
        </label>
        <label className="min-w-[8rem] flex-[1_1_9rem]">
          <span className="label m-0 block">In</span>
          <select name="field" className="w-full" value={f} onChange={(e) => setF(e.target.value)}>
            <option value="">all fields</option>
            {fields.map((name) => (
              <option key={name}>{name}</option>
            ))}
          </select>
        </label>
        <label className="w-[5.5rem]">
          <span className="label m-0 block">Hits</span>
          <input name="top_k" type="number" min={1} max={100} className="w-full" value={k} onChange={(e) => setK(e.target.value)} />
        </label>
        <Button type="submit" variant="primary">
          <Search size={14} aria-hidden /> Search
        </Button>
      </form>
      <div className="mt-3">
        {!asked ? (
          <Empty compact icon={Search} title="Ask the index something">
            Results appear here, ranked, with each matched row's fields and its score.
          </Empty>
        ) : search.isLoading ? (
          <Skeleton />
        ) : search.isError ? (
          <ErrorBlock error={search.error} retry={() => void search.refetch()} what="the search" />
        ) : hits.length === 0 ? (
          <Empty compact icon={SearchX} title="Nothing matched">
            Try other words, or search all fields.
          </Empty>
        ) : (
          <ol className="m-0 list-none divide-y divide-hairline rounded-[6px] border border-hairline p-0" aria-label="Search hits">
            {hits.map((hit, i) => (
              <li key={`${text(hit, "id")}-${i}`} className="grid grid-cols-[2rem_minmax(0,1fr)_minmax(6rem,10rem)] items-start gap-3 px-3 py-2">
                <span className="font-mono text-[12px] text-ink-3 tabular-nums">{i + 1}</span>
                <HitFields hit={hit} fields={fields} />
                <Meter value={Number(hit.score ?? 0) || 0} max={top || 1} label={cellText(hit.score ?? 0)} />
              </li>
            ))}
          </ol>
        )}
      </div>
    </Card>
  );
}

/** What a hit matched: the row's fields (zetta#1243), else, from an engine that sends none, the
 * workspace's row id. */
function HitFields({ hit, fields }: { hit: Row; fields: string[] }) {
  const pairs = hitFields(hit, fields);
  if (!pairs.length) {
    return (
      <code className="truncate" title={text(hit, "id")}>
        {text(hit, "id")}
      </code>
    );
  }
  return (
    <dl className="m-0 grid min-w-0 grid-cols-[minmax(0,auto)_minmax(0,1fr)] gap-x-3 gap-y-0.5 text-[13px]">
      {pairs.map(([name, value]) => (
        <div key={name} className="contents">
          <dt className="truncate font-mono text-[12px] text-ink-3">{name}</dt>
          <dd className="m-0 break-words text-ink">{value}</dd>
        </div>
      ))}
    </dl>
  );
}

/** Calling it from outside, the key: a key that can only search published indexes. */
function SearchKey({ table, published }: { table: string; published: boolean }) {
  const create = useAsync();
  const [secret, setSecret] = useState<string | null>(null);
  return (
    <Card label="Outside calls" title="Search Key">
      {!published ? (
        <p className="m-0 text-[13px] text-ink-2">Publish the index first: only published indexes answer outside calls.</p>
      ) : (
        <>
          <p className="mt-0 text-[13px] text-ink-2">Create a key that can only search published indexes, and copy it; it is shown once.</p>
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
              <KeyRound size={14} aria-hidden /> Create a search key
            </Button>
          )}
          <p className="mb-0 text-[12px] text-ink-3">
            The key keeps working for other published indexes until you revoke it on <Link to="/tokens">Tokens</Link>. A key expires no later than the session that made
            it would have, at most 30 days; signing out does not end it.
          </p>
        </>
      )}
    </Card>
  );
}

type Lang = "curl" | "js" | "py";

/** Calling it from outside: the call itself, in curl, JavaScript and Python, and the MCP tool. */
function CallIt({ id, published }: { id: string; published: boolean }) {
  const { customerId, workspaceId } = useWorkspace();
  const origin = useOrigin();
  const [lang, setLang] = useState<Lang>("curl");
  if (!published) {
    return (
      <Card label="Outside calls" title="Call It from Your Code">
        <Banner tone="quiet">Publish the index first: only published indexes answer outside calls.</Banner>
      </Card>
    );
  }
  const workspace = workspaceId || "<workspace id>";
  const url = `${origin}/v1/search_index?customer_id=${encodeURIComponent(customerId)}&workspace_id=${encodeURIComponent(workspace)}&index_id=${encodeURIComponent(id)}&query=`;
  const curl = `curl -s -H "Authorization: Bearer $LAKE_SEARCH_KEY" \\\n  "${url}renewal%20risk&top_k=10"`;
  const js = `// JavaScript\nconst res = await fetch("${url}" + encodeURIComponent("renewal risk"), {\n  headers: { Authorization: \`Bearer \${process.env.LAKE_SEARCH_KEY}\` },\n});\nconst { value } = await res.json(); // value.hits: [{ id, score, fields }]`;
  const py = `# Python\nimport os, requests\nr = requests.get("${origin}/v1/search_index", params={"customer_id": "${customerId}", "workspace_id": "${workspace}", "index_id": "${id}", "query": "renewal risk"},\n    headers={"Authorization": f"Bearer {os.environ['LAKE_SEARCH_KEY']}"})\nhits = r.json()["value"]["hits"]  # each {"id", "score", "fields": the row's values}`;
  return (
    <Card label="Outside calls" title="Call It from Your Code">
      <p className="mt-0 text-[13px] text-ink-2">Call Lake with the search key. The search runs in your own workspace; Lake checks the key and records every query.</p>
      <StateTabs<Lang>
        value={lang}
        onChange={setLang}
        label="Language"
        tabs={[
          { value: "curl", label: "curl" },
          { value: "js", label: "JavaScript" },
          { value: "py", label: "Python" },
        ]}
      />
      <CopyBlock value={lang === "curl" ? curl : lang === "js" ? js : py} multiline />
      <p className="text-[13px] text-ink-2">
        From an AI agent, the same search is the MCP tool <code>search_index</code> at <code>{origin}/mcp</code>.
      </p>
      <Banner tone="quiet">Unpublishing the index stops every outside call at once.</Banner>
    </Card>
  );
}
