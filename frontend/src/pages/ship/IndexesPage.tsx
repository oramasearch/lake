/** Ship → Search Indexes (web_ui.cpp workspace_content "indexes", web_ui_new_index): the
 * workspace's search indexes and a form to make one over a dataset. The dataset choices, which the
 * server page deferred to /ship/indexes/datasets, are one ordinary read of the lean dataset list. */
import { useState } from "react";
import { Link, useNavigate } from "react-router-dom";
import { type Row } from "@/lib/api";
import { useTrail } from "@/lib/trail";
import { Button, DataTable, ErrorBlock, Field, Form, Query, fieldError } from "@/lib/ui";
import { annotateDatasets, useLeanDatasets } from "../train/datasets";
import { WorkspaceFrame, cellText, qs, text, useWorkspace, useWq, useWsPath, wa } from "../train/workspace";

const SUB = "Search indexes over your data, and whether each is published for queries.";

export default function IndexesPage() {
  const { customerId } = useWorkspace();
  const ws = useWsPath();
  const space = qs({ customer_id: customerId });
  useTrail([
    { label: "Ship", to: ws(`/ship/models?${space}`) },
    { label: "Search Indexes", to: ws(`/ship/indexes?${space}`) },
  ]);
  return (
    <WorkspaceFrame eyebrow="Ship" title="Search Indexes" sub={SUB}>
      {() => <Indexes />}
    </WorkspaceFrame>
  );
}

function Indexes() {
  const { customerId } = useWorkspace();
  const ws = useWsPath();
  const space = qs({ customer_id: customerId });
  const indexes = useWq<{ indexes?: Row[] }>("list_indexes");
  const link = (id: string) => ws(`/ship/index?${qs({ customer_id: customerId, id })}`);
  return (
    <>
      <Query q={indexes} what="search indexes" isEmpty={(v) => !(v.indexes ?? []).length} empty={<div className="panel empty">No search indexes yet.</div>}>
        {(value) => (
          <>
            <DataTable
              rows={value.indexes ?? []}
              rowKey={(row) => text(row, "id")}
              caption="Search indexes"
              columns={[
                { key: "id", label: "Index", render: (row) => <Link to={link(text(row, "id"))}>{text(row, "id")}</Link> },
                { key: "table", label: "Table", render: (row) => text(row, "table") },
                { key: "kind", label: "Kind", render: (row) => text(row, "kind") },
                { key: "fields", label: "Fields", render: (row) => (Array.isArray(row.fields) ? row.fields.map(cellText).join(", ") : "") },
                { key: "created", label: "Created", render: (row) => text(row, "created") },
                { key: "open", label: "", render: (row) => <Link to={link(text(row, "id"))}>Search</Link> },
              ]}
            />
            <p className="muted">Open an index to search it, publish it, and call it from your own code.</p>
          </>
        )}
      </Query>
      <NewIndex space={space} />
    </>
  );
}

function NewIndex({ space }: { space: string }) {
  const scope = useWorkspace();
  const ws = useWsPath();
  const navigate = useNavigate();
  const listed = useLeanDatasets();
  const [table, setTable] = useState("");
  const [fields, setFields] = useState("");
  const [error, setError] = useState<unknown>(null);
  const [busy, setBusy] = useState(false);
  if (listed.isLoading) {
    return (
      <section className="panel" id="new-index" aria-busy="true">
        <h2>New Index</h2>
        <p className="muted" role="status">
          Loading your datasets…
        </p>
      </section>
    );
  }
  if (listed.isError) {
    return (
      <section className="panel" id="new-index">
        <h2>New Index</h2>
        <ErrorBlock error={listed.error} retry={() => void listed.refetch()} what="your datasets" />
      </section>
    );
  }
  const datasets = annotateDatasets(listed.data);
  const data = datasets.filter((d) => d.kind !== "result");
  const results = datasets.filter((d) => d.kind === "result");
  if (!datasets.length) {
    return (
      <p className="muted" id="new-index">
        To make an index, first add a dataset on <Link to={ws(`/train/data?${space}`)}>Data</Link>.
      </p>
    );
  }
  const chosen = table || text(data[0] ?? results[0], "name");
  const submit = async () => {
    setBusy(true);
    setError(null);
    try {
      const list = fields
        .split(",")
        .map((f) => f.trim())
        .filter(Boolean);
      const created = await wa<{ index_id?: string }>(scope, "create_index", { table: chosen, ...(list.length ? { fields: list } : {}) });
      navigate(ws(`/ship/index?${qs({ customer_id: scope.customerId, id: created.index_id ?? "" })}`));
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };
  return (
    <section className="panel" id="new-index">
      <h2>New Index</h2>
      <Form onSubmit={submit} error={fieldError(error, "table") || fieldError(error, "fields") ? undefined : error}>
        <Field label="Dataset" error={fieldError(error, "table")}>
          <select name="table" value={chosen} onChange={(e) => setTable(e.target.value)}>
            {data.map((d) => (
              <option key={text(d, "id")}>{text(d, "name")}</option>
            ))}
            {results.length > 0 && (
              <optgroup label="Results">
                {results.map((d) => (
                  <option key={text(d, "id")}>{text(d, "name")}</option>
                ))}
              </optgroup>
            )}
          </select>
        </Field>
        <Field label="Fields to search, comma separated" error={fieldError(error, "fields")}>
          <input name="fields" placeholder="all columns" value={fields} onChange={(e) => setFields(e.target.value)} />
        </Field>
        <p className="muted">A full-text (BM25F) index over the dataset.</p>
        <div>
          <Button type="submit" variant="primary" loading={busy}>
            Create index
          </Button>
        </div>
      </Form>
    </section>
  );
}
