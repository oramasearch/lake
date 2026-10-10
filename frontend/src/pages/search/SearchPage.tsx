/** Search (`/search?query=&customer=&repository=&ref=&change=&history=1&workspace_id=`): ranked,
 * substring or regex search across the repositories the viewer may read (web_ui_search,
 * serve_search_page). The filter form is the URL. */
import { useSearchParams } from "react-router-dom";
import { useRead, type Row } from "@/lib/api";
import { DataTable, ErrorBlock, Page, Skeleton } from "@/lib/ui";
import { PanelEmpty, useMetaTrail, useQuerySubmit } from "@/pages/organizations/shared";

export default function SearchPage() {
  const [params] = useSearchParams();
  const text = (name: string) => params.get(name) ?? "";
  const query = text("query");
  const history = text("history") === "1";
  const result = useRead<Row>(query ? "search" : null, {
    query,
    limit: 100,
    customer: text("customer"),
    repository: text("repository"),
    ref: text("ref"),
    change: text("change"),
    history: history ? true : undefined,
    // Kept only when the URL names it: the form has no field for it.
    workspace_id: text("workspace_id"),
  });
  const submit = useQuerySubmit("/search");
  useMetaTrail(query || null, "Search");
  const r = result.data;
  const items: Row[] = r?.items ?? [];
  return (
    <Page
      eyebrow="search"
      title="Search"
      sub={
        <>
          Ranked text, quoted substring, or /regex/ search across repositories you may read. A tag uses its immutable snapshot; a change or complete history uses scan mode.
          Append <code>in:artifact:&lt;name&gt;@&lt;version&gt;</code> to search a published artifact.
        </>
      }
    >
      <form className="filters" onSubmit={submit} key={params.toString()}>
        {text("workspace_id") && <input type="hidden" name="workspace_id" defaultValue={text("workspace_id")} />}
        <label>
          Query
          <input name="query" defaultValue={query} placeholder="Search repositories" required />
        </label>
        <label>
          Customer
          <input name="customer" defaultValue={text("customer")} />
        </label>
        <label>
          Repository
          <input name="repository" defaultValue={text("repository")} />
        </label>
        <label>
          Branch or tag
          <input name="ref" defaultValue={text("ref")} />
        </label>
        <label>
          Exact change
          <input name="change" defaultValue={text("change")} />
        </label>
        <label>
          <input type="checkbox" name="history" value="1" defaultChecked={history} /> Scan complete history
        </label>
        <button type="submit">Search</button>
      </form>
      {!query ? (
        <PanelEmpty>Enter a query to search published changes.</PanelEmpty>
      ) : result.isError ? (
        <ErrorBlock error={result.error} retry={() => void result.refetch()} what="search results" />
      ) : !r ? (
        <Skeleton />
      ) : (
        <>
          <p>
            Query mode: <span className="badge">{r.mode}</span>
            {r.mode === "scan" && ` · ${r.scanned_changes ?? 0} changes examined`}.
          </p>
          <DataTable
            rows={items}
            rowKey={(_, index) => String(index)}
            empty={<PanelEmpty>No readable matches.</PanelEmpty>}
            columns={[
              { key: "repository", label: "Repository", render: (h) => `${h.customer}/${h.repository}` },
              { key: "artifact", label: "Artifact", render: (h) => <code>{h.artifact ? `${h.artifact}@${h.artifact_version}` : "—"}</code> },
              { key: "path", label: "Path", render: (h) => <code>{h.path}</code> },
              { key: "line", label: "Line", render: (h) => (h.line ? h.line : "—") },
              { key: "change", label: "Change", render: (h) => <code>{h.change}</code> },
              { key: "snippet", label: "Snippet", render: (h) => h.snippet },
            ]}
          />
        </>
      )}
    </Page>
  );
}
