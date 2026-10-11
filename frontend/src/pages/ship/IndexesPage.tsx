/** Ship → Search Indexes (web_ui.cpp workspace_content "indexes", web_ui_new_index): the
 * workspace's search indexes and a form, in a drawer, to make one over a dataset. The dataset
 * choices, which the server page deferred to /ship/indexes/datasets, are one ordinary read of the
 * lean dataset list. */
import { useMemo, useState } from "react";
import { Link, useNavigate } from "react-router-dom";
import { Database, FileSearch, Plus, Search, Table2 } from "lucide-react";
import { type Row } from "@/lib/api";
import { relative } from "@/lib/format";
import { Drawer } from "@/lib/overlay";
import { useTrail } from "@/lib/trail";
import { Button, Chip, DataTable, Empty, ErrorBlock, Field, FilterChips, Form, Query, SearchInput, Skeleton, Stat, StatRow, Toolbar, fieldError } from "@/lib/ui";
import { annotateDatasets, useLeanDatasets } from "../train/datasets";
import { WorkspaceFrame, cellText, qs, text, useWorkspace, useWq, useWsPath, wa } from "../train/workspace";

const SUB = "Search indexes over your data, and whether each is published for queries.";

export default function IndexesPage() {
  const { customerId } = useWorkspace();
  const ws = useWsPath();
  const space = qs({ customer_id: customerId });
  const [creating, setCreating] = useState(false);
  useTrail([
    { label: "Ship", to: ws(`/ship/models?${space}`) },
    { label: "Search Indexes", to: ws(`/ship/indexes?${space}`) },
  ]);
  return (
    <WorkspaceFrame
      eyebrow="Ship"
      title="Search Indexes"
      sub={SUB}
      actions={
        <Button variant="primary" onClick={() => setCreating(true)}>
          <Plus size={14} aria-hidden /> New index
        </Button>
      }
      always={creating ? <NewIndex space={space} onClose={() => setCreating(false)} /> : null}
    >
      {() => <Indexes onNew={() => setCreating(true)} />}
    </WorkspaceFrame>
  );
}

function fieldsOf(row: Row): string[] {
  return Array.isArray(row.fields) ? row.fields.map(cellText) : [];
}

function Indexes({ onNew }: { onNew: () => void }) {
  const { customerId } = useWorkspace();
  const ws = useWsPath();
  const indexes = useWq<{ indexes?: Row[] }>("list_indexes");
  const link = (id: string) => ws(`/ship/index?${qs({ customer_id: customerId, id })}`);
  const [needle, setNeedle] = useState("");
  const [kind, setKind] = useState<string | null>(null);
  const kinds = useMemo(() => {
    const counts = new Map<string, number>();
    for (const row of indexes.data?.indexes ?? []) counts.set(text(row, "kind"), (counts.get(text(row, "kind")) ?? 0) + 1);
    return [...counts.entries()].filter(([id]) => id).map(([id, count]) => ({ id, label: id, count }));
  }, [indexes.data]);
  return (
    <Query
      q={indexes}
      what="search indexes"
      lines={6}
      isEmpty={(v) => !(v.indexes ?? []).length}
      empty={
        <Empty
          icon={FileSearch}
          title="No search indexes yet"
          action={
            <Button variant="primary" onClick={onNew}>
              <Plus size={14} aria-hidden /> New index
            </Button>
          }
        >
          Make one over a dataset, then search it here and from your own code.
        </Empty>
      }
    >
      {(value) => {
        const all = value.indexes ?? [];
        const tables = new Set(all.map((row) => text(row, "table")).filter(Boolean));
        const lower = needle.trim().toLowerCase();
        const rows = all.filter(
          (row) =>
            (!lower || [text(row, "id"), text(row, "table"), text(row, "kind"), text(row, "backend"), ...fieldsOf(row)].some((s) => s.toLowerCase().includes(lower))) &&
            (!kind || text(row, "kind") === kind),
        );
        return (
          <>
            <StatRow cols={3}>
              <Stat label="Indexes" value={all.length} icon={FileSearch} />
              <Stat label="Datasets covered" value={tables.size} icon={Table2} />
              <Stat label="Fields indexed" value={all.reduce((n, row) => n + fieldsOf(row).length, 0)} icon={Database} sub="across every index" />
            </StatRow>
            <Toolbar>
              <SearchInput value={needle} onChange={setNeedle} placeholder="Search indexes…" label="Search indexes" />
              {kinds.length > 1 && <FilterChips options={kinds} value={kind} onChange={setKind} allLabel="Every kind" />}
            </Toolbar>
            <DataTable
              rows={rows}
              rowKey={(row) => text(row, "id")}
              caption="Search indexes"
              empty={<Empty compact title="No index matches">Clear the search or a filter to see more.</Empty>}
              initialSort={{ key: "created", dir: "descending" }}
              columns={[
                {
                  key: "id",
                  label: "Index",
                  sort: (row) => text(row, "table").toLowerCase(),
                  render: (row) => (
                    <div className="min-w-0">
                      <Link to={link(text(row, "id"))} className="font-semibold">
                        {text(row, "table") || text(row, "id")}
                      </Link>
                      <div className="text-[12px] text-ink-3">
                        <code title={text(row, "id")}>{text(row, "id").slice(0, 8)}</code>
                        {text(row, "backend") && <> · {text(row, "backend")}</>}
                      </div>
                    </div>
                  ),
                },
                { key: "kind", label: "Kind", sort: (row) => text(row, "kind"), render: (row) => (text(row, "kind") ? <Chip>{text(row, "kind")}</Chip> : <span className="muted">—</span>) },
                {
                  key: "fields",
                  label: "Fields",
                  sort: (row) => fieldsOf(row).length,
                  render: (row) => {
                    const fields = fieldsOf(row);
                    if (!fields.length) return <span className="muted">—</span>;
                    return (
                      <div className="flex flex-wrap gap-1" title={fields.join(", ")}>
                        {fields.slice(0, 4).map((f) => (
                          <Chip key={f}>{f}</Chip>
                        ))}
                        {fields.length > 4 && <Chip>+{fields.length - 4}</Chip>}
                      </div>
                    );
                  },
                },
                {
                  key: "created",
                  label: "Created",
                  sort: (row) => text(row, "created"),
                  render: (row) => <span title={text(row, "created")}>{relative(text(row, "created"), Date.now(), "—")}</span>,
                },
                {
                  key: "open",
                  label: <span className="sr-only">Open</span>,
                  render: (row) => (
                    <Button variant="ghost" to={link(text(row, "id"))}>
                      <Search size={14} aria-hidden /> Search
                    </Button>
                  ),
                },
              ]}
            />
            <p className="muted mt-2 text-[12.5px]">Open an index to search it, publish it, and call it from your own code.</p>
          </>
        );
      }}
    </Query>
  );
}

function NewIndex({ space, onClose }: { space: string; onClose: () => void }) {
  const scope = useWorkspace();
  const ws = useWsPath();
  const navigate = useNavigate();
  const listed = useLeanDatasets();
  const [table, setTable] = useState("");
  const [fields, setFields] = useState("");
  const [error, setError] = useState<unknown>(null);
  const [busy, setBusy] = useState(false);
  const datasets = annotateDatasets(listed.data);
  const data = datasets.filter((d) => d.kind !== "result");
  const results = datasets.filter((d) => d.kind === "result");
  const chosen = table || text(data[0] ?? results[0], "name");
  const ready = listed.isSuccess && datasets.length > 0;
  const submit = async () => {
    setBusy(true);
    setError(null);
    try {
      const list = fields
        .split(",")
        .map((f) => f.trim())
        .filter(Boolean);
      const created = await wa<{ index_id?: string }>(scope, "create_index", { table: chosen, ...(list.length ? { fields: list } : {}) });
      onClose();
      navigate(ws(`/ship/index?${qs({ customer_id: scope.customerId, id: created.index_id ?? "" })}`));
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };
  return (
    <Drawer
      title="New Index"
      subtitle="A full-text (BM25F) index over the dataset."
      onClose={onClose}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          {ready && (
            <Button type="submit" form="new-index" variant="primary" loading={busy}>
              Create index
            </Button>
          )}
        </>
      }
    >
      {listed.isLoading ? (
        <div aria-busy="true">
          <p className="muted" role="status">
            Loading your datasets…
          </p>
          <Skeleton lines={2} />
        </div>
      ) : listed.isError ? (
        <ErrorBlock error={listed.error} retry={() => void listed.refetch()} what="your datasets" />
      ) : !datasets.length ? (
        <Empty
          compact
          icon={Database}
          title="No datasets yet"
          action={
            <Button to={ws(`/train/data?${space}`)} variant="primary">
              Go to Data
            </Button>
          }
        >
          To make an index, first add a dataset on Data.
        </Empty>
      ) : (
        <Form id="new-index" onSubmit={submit} error={fieldError(error, "table") || fieldError(error, "fields") ? undefined : error}>
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
          <Field label="Fields to search, comma separated" help="Leave it empty to search all columns." error={fieldError(error, "fields")}>
            <input name="fields" placeholder="all columns" value={fields} onChange={(e) => setFields(e.target.value)} />
          </Field>
        </Form>
      )}
    </Drawer>
  );
}
