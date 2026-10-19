/** Data → Query (web_ui.cpp workspace_content "query"): read-only SQL over the workspace's data.
 * The statement and database stay in the URL, so a result is linkable. */
import { useState } from "react";
import { useSearchParams } from "react-router-dom";
import { Database, Play, Rows3 } from "lucide-react";
import type { Row } from "@/lib/api";
import { useTrail } from "@/lib/trail";
import { Banner, Button, Card, Empty, ErrorBlock, Kbd, Skeleton } from "@/lib/ui";
import { WorkspaceFrame, qs, useSpaceRead, useWorkspace, useWsPath } from "./workspace";
import { ObjectTable } from "./datasets";
import { cappedStatement, limitRefused } from "./columns";

/** The most rows the page shows. The engine is asked for one more (`LIMIT`, zetta#1021), so the page
 * can say there are more without reading the whole table. */
const SHOWN = 500;

export default function QueryPage() {
  const { customerId } = useWorkspace();
  const ws = useWsPath();
  const space = qs({ customer_id: customerId });
  useTrail([{ label: "Data", to: "/repositories" }, { label: "Datasets", to: ws(`/train/data?${space}`) }, { label: "Query" }]);
  const [params] = useSearchParams();
  const error = params.get("error");
  return (
    <WorkspaceFrame
      eyebrow="Data"
      title="Query"
      sub="Read-only SQL over your workspace's data: one SELECT or WITH query at a time."
      actions={
        <Button to={ws(`/train/data?${space}`)}>
          <Database size={14} aria-hidden /> Datasets
        </Button>
      }
      before={error ? <Banner tone="warn">{error}</Banner> : null}
    >
      {() => <QueryContent />}
    </WorkspaceFrame>
  );
}

function QueryContent() {
  const [params, setParams] = useSearchParams();
  const statement = params.get("statement") ?? "";
  const database = params.get("database") ?? "";
  const [draft, setDraft] = useState({ statement, database });
  const [shown, setShown] = useState({ statement, database });
  // Typing keeps the draft; a new URL (back, a link) shows its own statement.
  if (shown.statement !== statement || shown.database !== database) {
    setShown({ statement, database });
    setDraft({ statement, database });
  }
  // A plain SELECT with no LIMIT of its own is sent with `LIMIT 501`. An engine older than zetta#1039
  // refuses LIMIT; then the statement goes as written and the page shows the first rows itself.
  const capped = cappedStatement(statement, SHOWN);
  const first = useSpaceRead<{ columns?: string[]; rows?: Row[] }>("workspace_sql", { statement: capped, database: database || undefined }, { enabled: !!statement });
  const asWritten = capped !== statement && first.isError && limitRefused(first.error);
  const whole = useSpaceRead<{ columns?: string[]; rows?: Row[] }>("workspace_sql", { statement, database: database || undefined }, { enabled: !!statement && asWritten });
  const result = asWritten ? whole : first;
  const rows = result.data?.rows ?? [];
  // With the cap, more than SHOWN rows back means the statement has more than that many.
  const more = rows.length > SHOWN;
  const counted = capped !== statement && !asWritten && more ? `More than ${SHOWN.toLocaleString("en-US")} rows` : `${rows.length.toLocaleString("en-US")} rows`;
  const run = () => {
    const next = new URLSearchParams(params);
    next.delete("error");
    next.set("statement", draft.statement);
    if (draft.database) next.set("database", draft.database);
    else next.delete("database");
    setParams(next);
  };
  return (
    <div className="space-y-4">
      <Card padded={false}>
        <form
          onSubmit={(e) => {
            e.preventDefault();
            run();
          }}
        >
          <div className="flex flex-wrap items-center gap-2 border-b border-hairline px-4 py-2.5">
            <span className="label m-0">SQL</span>
            <span className="muted ml-auto text-[12px]">
              <Kbd>⌘</Kbd> <Kbd>Enter</Kbd> runs it
            </span>
          </div>
          <label className="sr-only" htmlFor="query-statement">
            Query
          </label>
          <textarea
            id="query-statement"
            name="statement"
            rows={7}
            required
            spellCheck={false}
            className="block min-h-[10rem] w-full resize-y rounded-none border-0 bg-transparent px-4 py-3 font-mono text-[13px] focus:outline-none"
            placeholder="SELECT plan, COUNT(*) FROM datasets.accounts GROUP BY plan"
            value={draft.statement}
            onChange={(e) => setDraft({ ...draft, statement: e.target.value })}
            onKeyDown={(e) => {
              if (e.key === "Enter" && (e.metaKey || e.ctrlKey) && draft.statement.trim()) {
                e.preventDefault();
                run();
              }
            }}
          />
          <div className="flex flex-wrap items-end gap-3 border-t border-hairline px-4 py-3">
            <label className="flex min-w-[12rem] flex-1 flex-col gap-1 text-[12.5px] text-ink-2 sm:max-w-[16rem] sm:flex-none">
              Database
              <input name="database" placeholder="datasets" value={draft.database} onChange={(e) => setDraft({ ...draft, database: e.target.value })} />
            </label>
            <div className="ml-auto">
              <Button variant="primary" type="submit" loading={result.isFetching}>
                <Play size={14} aria-hidden /> Run
              </Button>
            </div>
          </div>
        </form>
      </Card>
      {statement && result.isLoading && <Skeleton />}
      {statement && result.isError && <ErrorBlock error={result.error} what="the query" />}
      {statement && result.data && (
        <Card label="Results" title={counted}>
          {more && <Banner tone="quiet">Showing the first {SHOWN}. Add LIMIT and OFFSET to page through the rest.</Banner>}
          {rows.length === 0 ? (
            <Empty compact icon={Rows3} title="No rows">
              The query matched nothing.
            </Empty>
          ) : (
            <ObjectTable rows={rows.slice(0, SHOWN)} columns={result.data.columns} />
          )}
        </Card>
      )}
      {!statement && (
        <Empty icon={Play} title="Run a query">
          Write one SELECT or WITH query above; its rows appear here and the address keeps it, so you can share the link.
        </Empty>
      )}
    </div>
  );
}
