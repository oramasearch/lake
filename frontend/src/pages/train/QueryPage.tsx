/** Train → Query (web_ui.cpp workspace_content "query"): read-only SQL over the workspace's data.
 * The statement and database stay in the URL, so a result is linkable. */
import { useState } from "react";
import { useSearchParams } from "react-router-dom";
import type { Row } from "@/lib/api";
import { useTrail } from "@/lib/trail";
import { Button, ErrorBlock, Skeleton } from "@/lib/ui";
import { WorkspaceFrame, qs, useSpaceRead, useWorkspace, useWsPath } from "./workspace";
import { ObjectTable } from "./datasets";

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
      before={
        error ? (
          <p className="error" role="alert">
            {error}
          </p>
        ) : null
      }
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
  const result = useSpaceRead<{ rows?: Row[] }>("workspace_sql", { statement, database: database || undefined }, { enabled: !!statement });
  const rows = result.data?.rows ?? [];
  return (
    <>
      <section className="panel">
        <form
          className="stack"
          onSubmit={(e) => {
            e.preventDefault();
            const next = new URLSearchParams(params);
            next.delete("error");
            next.set("statement", draft.statement);
            if (draft.database) next.set("database", draft.database);
            else next.delete("database");
            setParams(next);
          }}
        >
          <label>
            Query
            <textarea
              name="statement"
              rows={5}
              required
              placeholder="SELECT plan, COUNT(*) FROM datasets.accounts GROUP BY plan"
              value={draft.statement}
              onChange={(e) => setDraft({ ...draft, statement: e.target.value })}
            />
          </label>
          <label>
            Database
            <input name="database" placeholder="datasets" value={draft.database} onChange={(e) => setDraft({ ...draft, database: e.target.value })} />
          </label>
          <Button variant="primary" type="submit" loading={result.isFetching}>
            Run
          </Button>
        </form>
      </section>
      {statement && result.isLoading && <Skeleton />}
      {statement && result.isError && <ErrorBlock error={result.error} what="the query" />}
      {statement && result.data && (
        <section className="panel">
          <h2>{rows.length} rows</h2>
          {rows.length > 500 && <p className="sub">Showing the first 500.</p>}
          {rows.length === 0 ? <p>No rows.</p> : <ObjectTable rows={rows.slice(0, 500)} />}
        </section>
      )}
    </>
  );
}
