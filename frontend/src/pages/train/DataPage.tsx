/** Data → Datasets (web_ui.cpp workspace_content "data", storage_content): the workspace's datasets
 * with figures and a toolbar, Load data (upload, generate, pull from a connection) in a drawer, the
 * workspace's connections and its storage. Every form handler of /train/data/* is a capability call
 * here. */
import { useEffect, useMemo, useState } from "react";
import { Link, useNavigate, useSearchParams } from "react-router-dom";
import { Cable, Database, HardDrive, Plus, Sparkles, Table2, Terminal, Upload as UploadIcon } from "lucide-react";
import { act, type Row } from "@/lib/api";
import { bytes } from "@/lib/format";
import { ConfirmButton, Drawer, Modal } from "@/lib/overlay";
import { useToast } from "@/lib/toast";
import { useTrail, entityCrumb } from "@/lib/trail";
import {
  Banner,
  Button,
  Card,
  Chip,
  DataTable,
  Empty,
  ErrorBlock,
  Field,
  FilePicker,
  FilterChips,
  Form,
  Mono,
  Query,
  SearchInput,
  Stat,
  StateTabs,
  StatRow,
  Status,
  Toolbar,
  type Tone,
} from "@/lib/ui";
import { WorkspaceFrame, qs, useAsync, useSpaceRead, useWorkspace, timeAgo, useWorkspaceReady, useWq, useWsPath, wa, text } from "./workspace";
import { annotateDatasets, resultDataset, useLeanDatasets } from "./datasets";

const SUB = "The connections and datasets your workspace trains on.";
const NAME_PATTERN = "[A-Za-z_][A-Za-z0-9_]{0,62}";
const UPLOAD_LIMIT = 16 * 1024 * 1024;

type Open = null | "load" | "connection";

export default function DataPage() {
  const [params] = useSearchParams();
  const browse = params.get("browse") ?? "";
  const { customerId } = useWorkspace();
  const ws = useWsPath();
  const ready = useWorkspaceReady();
  const space = qs({ customer_id: customerId });
  const [open, setOpen] = useState<Open>(null);
  useTrail([
    { label: "Data", to: "/repositories" },
    { label: "Datasets", to: ws(`/train/data?${space}`) },
    ...(browse ? [entityCrumb(browse)] : []),
  ]);
  const error = params.get("error");
  return (
    <>
      <WorkspaceFrame
        eyebrow="Data"
        title="Datasets"
        sub={SUB}
        actions={
          ready ? (
            <>
              <Button to={ws(`/train/query?${space}`)}>
                <Terminal size={14} aria-hidden /> Query with SQL
              </Button>
              <Button variant="primary" onClick={() => setOpen("load")}>
                <UploadIcon size={14} aria-hidden /> Load data
              </Button>
            </>
          ) : null
        }
        before={error ? <Banner tone="warn">{error}</Banner> : null}
        always={<Storage />}
      >
        {() => <DataContent browse={browse} onOpen={setOpen} />}
      </WorkspaceFrame>
      {open === "load" && <LoadData onClose={() => setOpen(null)} onAddConnection={() => setOpen("connection")} />}
      {open === "connection" && <AddConnection onClose={() => setOpen(null)} />}
    </>
  );
}

/** The workspace's stores, checked every 3 seconds while one is changing (shared by the figures and
 * the Storage card: one read). */
function useStores() {
  return useSpaceRead<{ stores?: Row[] }>("list_stores", {}, {
    refetchInterval: (query) => {
      const list = ((query.state.data as { stores?: Row[] } | undefined)?.stores ?? []) as Row[];
      return list.some((s) => !["Ready", "Failed"].includes(storePhase(s))) ? 3_000 : false;
    },
  });
}

function DataContent({ browse, onOpen }: { browse: string; onOpen: (open: Open) => void }) {
  const [params] = useSearchParams();
  const { customerId } = useWorkspace();
  const ws = useWsPath();
  const connections = useWq<{ connections?: Row[] }>("list_connections");
  const datasets = useLeanDatasets();
  const stores = useStores();
  const services = useSpaceRead<{ services?: Row[] }>("list_services", {}, { workspace: false });
  const tables = useWq<{ tables?: Row[] }>(browse ? "list_source_tables" : null, { connection: browse, include_columns: true, max_tables: 100 });
  const connectionList = connections.data?.connections ?? [];
  const serviceList = services.data?.services ?? [];
  const loaded = params.get("loaded");

  const named = useMemo(() => annotateDatasets(datasets.data).filter((d) => text(d, "name")), [datasets.data]);
  const own = named.filter((d) => !resultDataset(d));
  const results = named.filter((d) => resultDataset(d));
  const storeList = stores.data?.stores ?? [];
  const changing = storeList.filter((s) => !["Ready", "Failed"].includes(storePhase(s))).length;

  return (
    <>
      {loaded !== null && <LoadedNotice />}
      <StatRow>
        <Stat label="Datasets" icon={Database} value={datasets.data ? own.length : "—"} sub="you can train on" />
        <Stat label="Run outputs" icon={Table2} value={datasets.data ? results.length : "—"} sub="tables runs wrote" />
        <Stat label="Connections" icon={Cable} value={connections.data ? connectionList.length : "—"} sub="databases the workspace reads" />
        <Stat label="Stores" icon={HardDrive} value={stores.data ? storeList.length : "—"} sub={changing ? `${changing} changing` : "buckets, filesystems, volumes"} />
      </StatRow>

      <DatasetList datasets={datasets} own={own} results={results} onLoad={() => onOpen("load")} />

      <div className="mt-6 space-y-4">
        <Card
          label="Connections"
          title="Databases Your Data Lives In"
          actions={
            <Button onClick={() => onOpen("connection")}>
              <Plus size={14} aria-hidden /> Add connection
            </Button>
          }
        >
          <Query q={connections} what="connections">
            {(value) =>
              (value.connections ?? []).length === 0 ? (
                <Empty compact icon={Cable} title="No connections yet" action={<Button onClick={() => onOpen("connection")}>Add connection</Button>}>
                  Add the database your data lives in, or connect one of your services below.
                </Empty>
              ) : (
                <ConnectionsTable connections={value.connections ?? []} services={serviceList} />
              )
            }
          </Query>
          <Query q={services} what="services" lines={1}>
            {(value) => <ConnectServices services={value.services ?? []} connections={connectionList} />}
          </Query>
        </Card>

        {browse && (
          <Card
            label="Browse"
            title={`Tables in ${browse}`}
            actions={
              <Button variant="ghost" to={ws(`/train/data?${qs({ customer_id: customerId })}`)}>
                Close
              </Button>
            }
          >
            <Query q={tables} what="tables">
              {(value) =>
                (value.tables ?? []).length === 0 ? (
                  <Empty compact icon={Table2} title="No tables">
                    This connection shows no tables to its account.
                  </Empty>
                ) : (
                  <>
                    <DataTable<Row>
                      rows={value.tables ?? []}
                      what="tables"
                      columns={[
                        {
                          key: "table",
                          label: "Table",
                          sort: (t) => `${text(t, "schema")}.${text(t, "table")}`,
                          render: (t) => (
                            <div className="min-w-0">
                              <div className="font-semibold text-ink">{text(t, "table")}</div>
                              <div className="text-[12px] text-ink-3">{text(t, "schema")}</div>
                            </div>
                          ),
                        },
                        { key: "kind", label: "Kind", render: (t) => (text(t, "kind") ? <Chip>{text(t, "kind")}</Chip> : "—") },
                        {
                          key: "columns",
                          label: "Columns",
                          render: (t) => (
                            <span className="text-[12.5px] text-ink-2">
                              {((t.columns ?? []) as Row[]).map((c) => `${text(c, "column")} ${text(c, "type")}`).join(", ")}
                            </span>
                          ),
                        },
                      ]}
                    />
                    <p className="muted mt-2 mb-0 text-[12.5px]">
                      Pull one with <strong>Load data → From a connection</strong>.
                    </p>
                  </>
                )
              }
            </Query>
          </Card>
        )}
      </div>
    </>
  );
}

/** The datasets as a table: search, Datasets / Run outputs, and each dataset's link and query. */
function DatasetList({
  datasets,
  own,
  results,
  onLoad,
}: {
  datasets: ReturnType<typeof useLeanDatasets>;
  own: Row[];
  results: Row[];
  onLoad: () => void;
}) {
  const { customerId } = useWorkspace();
  const ws = useWsPath();
  const [search, setSearch] = useState("");
  // Run outputs are not offered as training data, so the list opens on the datasets.
  const [kind, setKind] = useState<"data" | "result" | null>("data");
  const all = [...own, ...results];
  const shown = (kind === "data" ? own : kind === "result" ? results : all).filter((d) => {
    const needle = search.trim().toLowerCase();
    return !needle || `${text(d, "name")} ${text(d, "database")}`.toLowerCase().includes(needle);
  });
  const hasRows = all.some((d) => typeof d.rows === "number");
  const hasSize = all.some((d) => typeof d.size_bytes === "number");
  const open = (d: Row) => ws(`/train/dataset?${qs({ customer_id: customerId, database: text(d, "database") || "datasets", name: text(d, "name") })}`);
  return (
    <section aria-label="Datasets">
      <Toolbar>
        <SearchInput value={search} onChange={setSearch} placeholder="Search datasets…" />
        <FilterChips
          value={kind}
          onChange={setKind}
          allLabel="All tables"
          options={[
            { id: "data", label: "Datasets", count: own.length },
            { id: "result", label: "Run outputs", count: results.length },
          ]}
        />
      </Toolbar>
      {kind === "result" && <Banner tone="quiet">Tables the engine wrote as the output of runs. They are not offered as training data.</Banner>}
      <DataTable<Row>
        q={datasets}
        what="datasets"
        rows={shown}
        rowKey={(d) => text(d, "id") || text(d, "name")}
        initialSort={{ key: "name", dir: "ascending" }}
        empty={
          all.length === 0 ? (
            <Empty
              icon={Database}
              title="No datasets yet"
              action={
                <Button variant="primary" onClick={onLoad}>
                  <UploadIcon size={14} aria-hidden /> Load data
                </Button>
              }
            >
              Upload a file, pull a table from a database you connected, or generate one. Each becomes a dataset you can train on.
            </Empty>
          ) : (
            <Empty compact title="No dataset matches">
              Clear the search or pick another filter.
            </Empty>
          )
        }
        columns={[
          {
            key: "name",
            label: "Dataset",
            sort: (d) => text(d, "name").toLowerCase(),
            render: (d) => (
              <div className="min-w-0">
                <Link to={open(d)} className="font-semibold">
                  {text(d, "name")}
                </Link>
                <div className="truncate font-mono text-[11.5px] text-ink-3">{`${text(d, "database") || "datasets"}/${text(d, "name")}`}</div>
              </div>
            ),
          },
          { key: "database", label: "Database", sort: (d) => text(d, "database"), render: (d) => <Chip>{text(d, "database") || "datasets"}</Chip> },
          {
            key: "kind",
            label: "Kind",
            sort: (d) => (resultDataset(d) ? 1 : 0),
            render: (d) => (resultDataset(d) ? <Chip tone="ai">Run output</Chip> : <Chip tone="ok">Training data</Chip>),
          },
          ...(hasRows ? [{ key: "rows", label: "Rows", numeric: true, sort: (d: Row) => (typeof d.rows === "number" ? d.rows : null), render: (d: Row) => <span className="font-mono">{typeof d.rows === "number" ? d.rows.toLocaleString("en-US") : "—"}</span> }] : []),
          ...(hasSize ? [{ key: "size", label: "Size", numeric: true, sort: (d: Row) => (typeof d.size_bytes === "number" ? d.size_bytes : null), render: (d: Row) => <span className="font-mono">{typeof d.size_bytes === "number" ? bytes(d.size_bytes) : "—"}</span> }] : []),
          {
            key: "query",
            label: "",
            className: "text-right",
            render: (d) => (
              <Button variant="ghost" to={ws(`/train/query?${qs({ customer_id: customerId, statement: `SELECT * FROM ${text(d, "database") || "datasets"}.${text(d, "name")}` })}`)}>
                Query
              </Button>
            ),
          },
        ]}
      />
      <p className="muted mt-2 mb-0 text-[12.5px]">Open a dataset for its columns and their types, row count, size and first rows.</p>
    </section>
  );
}

function LoadedNotice() {
  const [params] = useSearchParams();
  const { customerId } = useWorkspace();
  const ws = useWsPath();
  const table = params.get("loaded") ?? "";
  const database = params.get("database") || "datasets";
  const format = params.get("format") ?? "";
  const renamed = params.get("renamed_from") ?? "";
  return (
    <Banner
      tone="ok"
      title={
        <>
          Loaded {params.get("rows") ?? ""} rows {format === "csv" ? "from your CSV file " : format === "jsonl" ? "from your JSONL file " : ""}into{" "}
          <Link to={ws(`/train/dataset?${qs({ customer_id: customerId, database, name: table })}`)}>{table}</Link>.
        </>
      }
    >
      {renamed && renamed !== table ? (
        <>
          <strong>{renamed}</strong> was already taken, so it was saved as <strong>{table}</strong>.
        </>
      ) : null}
    </Banner>
  );
}

function ConnectionsTable({ connections, services }: { connections: Row[]; services: Row[] }) {
  const scope = useWorkspace();
  const ws = useWsPath();
  const toast = useToast();
  const space = qs({ customer_id: scope.customerId });
  return (
    <DataTable<Row>
      rows={connections}
      what="connections"
      rowKey={(c) => text(c, "name")}
      columns={[
        {
          key: "name",
          label: "Connection",
          sort: (c) => text(c, "name"),
          render: (connection) => {
            const connector = (connection.connector ?? {}) as Row;
            return (
              <div className="min-w-0">
                <div className="font-semibold text-ink">{text(connection, "name")}</div>
                <div className="text-[12px] text-ink-3">{text(connector, "type")}</div>
              </div>
            );
          },
        },
        {
          key: "where",
          label: "Where",
          render: (connection) => {
            const connector = (connection.connector ?? {}) as Row;
            const where = text(connector, "type") === "postgres" ? `${text(connector, "host")}/${text(connector, "database")}` : text(connector, "account");
            return <Mono>{where}</Mono>;
          },
        },
        {
          key: "service",
          label: "From service",
          render: (connection) => {
            const name = text(connection, "name");
            const host = text((connection.connector ?? {}) as Row, "host");
            const service = services.find((s) => text(s, "name") === name && host && text(s, "endpoint").startsWith(host));
            return service ? <Link to={`/services?${space}`}>{text(service, "name")}</Link> : <span className="muted">—</span>;
          },
        },
        {
          key: "saved",
          label: "Saved",
          sort: (c) => text(c, "created"),
          render: (c) => (
            <span className="font-mono text-[12px] text-ink-2" title={text(c, "created")}>
              {timeAgo(text(c, "created")) || "—"}
            </span>
          ),
        },
        {
          key: "actions",
          label: "",
          className: "text-right",
          render: (connection) => {
            const name = text(connection, "name");
            return (
              <div className="flex flex-wrap justify-end gap-1.5">
                <Button variant="ghost" to={ws(`/train/data?${qs({ customer_id: scope.customerId, browse: name })}`)}>
                  Browse tables
                </Button>
                <ConfirmButton
                  variant="ghost"
                  title={`Remove ${name}?`}
                  confirmLabel="Remove"
                  confirm={<>This removes the connection {name} from the workspace.</>}
                  onConfirm={async () => {
                    await wa(scope, "delete_connection", { name });
                    toast(`Removed ${name}`);
                  }}
                >
                  Remove
                </ConfirmButton>
              </div>
            );
          },
        },
      ]}
    />
  );
}

/** Connect One of Your Services (lake#188): one click, no password typed or shown. */
function ConnectServices({ services, connections }: { services: Row[]; connections: Row[] }) {
  const scope = useWorkspace();
  const toast = useToast();
  const connect = useAsync();
  const names = new Set(connections.map((c) => text(c, "name")));
  const rows = services.filter((s) => text(s, "desired") !== "absent" && !s.default && text(s, "kind") !== "zetta");
  if (rows.length === 0) return null;
  return (
    <div className="mt-4 border-t border-hairline pt-3">
      <div className="label m-0">Connect One of Your Services</div>
      <p className="muted mt-1 mb-2 text-[12.5px]">The workspace reaches it inside the cluster, with the credential lake already holds; nothing to type.</p>
      {connect.error ? <ErrorBlock error={connect.error} /> : null}
      <ul className="m-0 list-none divide-y divide-hairline p-0">
        {rows.map((service, i) => {
          const kind = text(service, "kind");
          const name = text(service, "name");
          const ready = text(service, "phase") === "Ready" && service.connected !== false;
          return (
            <li key={i} className="flex flex-wrap items-center gap-2 py-2">
              <span className="min-w-0 flex-1 font-semibold text-ink">{name}</span>
              <Chip>{kind}</Chip>
              {kind !== "postgres" ? (
                <span className="muted text-[12.5px]">The workspace can't read {kind} yet; Postgres works today</span>
              ) : !ready ? (
                <Status value="starting" />
              ) : (
                <Button
                  loading={connect.busy}
                  onClick={() =>
                    void connect.run(async () => {
                      await act("connect_service", { customer_id: scope.customerId, workspace_id: scope.workspaceId, service_id: text(service, "id") });
                      toast(`Connected ${name}`);
                    })
                  }
                >
                  {names.has(name) ? "Refresh" : "Connect"}
                </Button>
              )}
            </li>
          );
        })}
      </ul>
    </div>
  );
}

/* ------------------------------------------------------------------ load data */

/** Load data: upload a file, generate a dataset, or pull a table from a connection. */
function LoadData({ onClose, onAddConnection }: { onClose: () => void; onAddConnection: () => void }) {
  const [tab, setTab] = useState<"upload" | "generate" | "pull">("upload");
  const connections = useWq<{ connections?: Row[] }>("list_connections");
  const generators = useWq<{ generators?: Row[] }>("list_dataset_generators");
  // The primary action sits in the drawer's footer, as in every other drawer (lake#582); each tab's
  // form says whether it is busy, and a tab with nothing to act on shows no action.
  const [busy, setBusy] = useState(false);
  const action =
    tab === "upload"
      ? { form: "load-upload", label: "Upload", icon: UploadIcon }
      : tab === "generate" && (generators.data?.generators ?? []).length > 0
        ? { form: "load-generate", label: "Generate", icon: Sparkles }
        : tab === "pull" && (connections.data?.connections ?? []).length > 0
          ? { form: "load-pull", label: "Pull dataset", icon: null }
          : null;
  return (
    <Drawer
      title="Load Data"
      subtitle="Each becomes a dataset you can train on."
      onClose={onClose}
      width={560}
      footer={
        <>
          <Link to="/docs/load-data" className="mr-auto self-center text-[12.5px]">
            How loading data works
          </Link>
          <Button onClick={onClose}>Cancel</Button>
          {action && (
            <Button variant="primary" type="submit" form={action.form} loading={busy}>
              {action.icon && <action.icon size={14} aria-hidden />} {action.label}
            </Button>
          )}
        </>
      }
    >
      <StateTabs
        label="How to load"
        value={tab}
        onChange={setTab}
        tabs={[
          { value: "upload", label: "Upload" },
          { value: "generate", label: "Generate" },
          { value: "pull", label: "From a connection" },
        ]}
      />
      {tab === "upload" && <Upload onClose={onClose} onBusy={setBusy} />}
      {tab === "generate" && (
        <Query q={generators} what="dataset generators">
          {(value) =>
            (value.generators ?? []).length === 0 ? (
              <Empty compact icon={Sparkles} title="No generators">
                This workspace offers no dataset generators.
              </Empty>
            ) : (
              <Generate generators={value.generators ?? []} onDone={onClose} onBusy={setBusy} />
            )
          }
        </Query>
      )}
      {tab === "pull" && (
        <Query q={connections} what="connections">
          {(value) =>
            (value.connections ?? []).length === 0 ? (
              <Empty compact icon={Cable} title="No connections yet" action={<Button onClick={onAddConnection}>Add connection</Button>}>
                Connect the database your data lives in, then pull a table from it.
              </Empty>
            ) : (
              <Pull connections={value.connections ?? []} onDone={onClose} onBusy={setBusy} />
            )
          }
        </Query>
      )}
    </Drawer>
  );
}

function Upload({ onClose, onBusy }: { onClose: () => void; onBusy: (busy: boolean) => void }) {
  const scope = useWorkspace();
  const navigate = useNavigate();
  const ws = useWsPath();
  const upload = useAsync();
  useEffect(() => {
    onBusy(upload.busy);
  }, [upload.busy, onBusy]);
  const [file, setFile] = useState<File | null>(null);
  const [format, setFormat] = useState("");
  const [tableName, setTableName] = useState("");
  const [rename, setRename] = useState(false);
  return (
    <Form
      id="load-upload"
      error={upload.error}
      onSubmit={() =>
        upload.run(async () => {
          if (!file || file.size === 0) throw new Error("Choose one CSV or JSONL file.");
          if (file.size > UPLOAD_LIMIT) throw new Error("The file is larger than 16 MiB.");
          const chosen = format || (file.name.endsWith(".jsonl") || file.name.endsWith(".ndjson") ? "jsonl" : "csv");
          const content = await file.text();
          const result = await wa<Row>(scope, "upload_dataset", { table_name: tableName, format: chosen, content, if_exists: rename ? "rename" : "fail" });
          onClose();
          navigate(
            ws(
              `/train/data?${qs({
                customer_id: scope.customerId,
                loaded: text(result, "table"),
                rows: typeof result.rows === "number" ? result.rows : 0,
                database: text(result, "database") || "datasets",
                format: text(result, "format") || chosen,
                renamed_from: text(result, "renamed_from"),
              })}`,
            ),
          );
        })
      }
    >
      <div>
        <span className="mb-1 block">File</span>
        <FilePicker
          file={file}
          onFile={setFile}
          name="file"
          accept=".csv,.jsonl,.ndjson,text/csv"
          prompt="Choose a CSV or JSONL file, or drop it here"
          hint="Up to 16 MiB"
        />
      </div>
      <label>
        Format
        <select name="format" value={format} onChange={(e) => setFormat(e.target.value)}>
          <option value="">From the file name</option>
          <option value="csv">CSV</option>
          <option value="jsonl">JSONL</option>
        </select>
      </label>
      <label>
        Dataset name
        <input name="table_name" required pattern={NAME_PATTERN} placeholder="accounts" value={tableName} onChange={(e) => setTableName(e.target.value)} />
        <small className="muted">Letters, digits and underscores, starting with a letter, so SQL can query it.</small>
      </label>
      <label className="flex! items-start gap-2">
        <input type="checkbox" name="if_exists" value="rename" checked={rename} onChange={(e) => setRename(e.target.checked)} />
        <span>If the name is taken, save it under the next free name (accounts_2)</span>
      </label>
      <Banner tone="quiet">Otherwise a taken name is refused, and a CSV row whose field count differs from the header is refused with its line number.</Banner>
    </Form>
  );
}

function Pull({ connections, onDone, onBusy }: { connections: Row[]; onDone: () => void; onBusy: (busy: boolean) => void }) {
  const scope = useWorkspace();
  const toast = useToast();
  const pull = useAsync();
  useEffect(() => {
    onBusy(pull.busy);
  }, [pull.busy, onBusy]);
  const [connection, setConnection] = useState(text(connections[0], "name"));
  const [query, setQuery] = useState("");
  const [tableName, setTableName] = useState("");
  const [maxRows, setMaxRows] = useState("");
  return (
    <Form
      id="load-pull"
      error={pull.error}
      onSubmit={() =>
        pull.run(async () => {
          const input: Row = { connection, query, table_name: tableName };
          if (maxRows) {
            if (!/^\d+$/.test(maxRows) || Number(maxRows) < 1) throw new Error("The row limit must be a whole number.");
            input.max_rows = Number(maxRows);
          }
          try {
            await wa(scope, "create_dataset_from_query", input);
          } catch (e) {
            // A workspace started with --governance asks for an approval lake cannot give.
            if ((e as { code?: string }).code === "consent_required")
              throw new Error("This workspace requires an approval before it reaches an outside database, and lake cannot approve it.");
            throw e;
          }
          toast(`Pulled ${tableName}`);
          setQuery("");
          setTableName("");
          setMaxRows("");
          onDone();
        })
      }
    >
      <label>
        From
        <select name="connection" value={connection} onChange={(e) => setConnection(e.target.value)}>
          {connections.map((c, i) => (
            <option key={i}>{text(c, "name")}</option>
          ))}
        </select>
      </label>
      <label>
        Query
        <textarea
          name="query"
          rows={4}
          required
          className="font-mono"
          placeholder="SELECT account_id, plan, seats, renewed FROM accounts"
          value={query}
          onChange={(e) => setQuery(e.target.value)}
        />
      </label>
      <label>
        Dataset name
        <input name="table_name" required pattern={NAME_PATTERN} placeholder="accounts" value={tableName} onChange={(e) => setTableName(e.target.value)} />
      </label>
      <label>
        At most this many rows
        <input name="max_rows" inputMode="numeric" placeholder="100000" value={maxRows} onChange={(e) => setMaxRows(e.target.value)} />
      </label>
    </Form>
  );
}

function Generate({ generators, onDone, onBusy }: { generators: Row[]; onDone: () => void; onBusy: (busy: boolean) => void }) {
  const scope = useWorkspace();
  const toast = useToast();
  const generate = useAsync();
  useEffect(() => {
    onBusy(generate.busy);
  }, [generate.busy, onBusy]);
  const [kind, setKind] = useState(text(generators[0], "kind"));
  const [variants, setVariants] = useState("");
  return (
    <Form
      id="load-generate"
      error={generate.error}
      onSubmit={() =>
        generate.run(async () => {
          // Never `path`: it would write a file on the workspace's node.
          const input: Row = { kind };
          if (variants) input.variants_per_subject = Math.min(50, Math.max(1, parseInt(variants, 10) || 0));
          await wa(scope, "generate_dataset", input);
          toast("Dataset generated");
          onDone();
        })
      }
    >
      <label>
        Generator
        <select name="kind" value={kind} onChange={(e) => setKind(e.target.value)}>
          {generators.map((g, i) => (
            <option key={i} value={text(g, "kind")}>
              {text(g, "title") || text(g, "kind")}
            </option>
          ))}
        </select>
      </label>
      <label>
        Variants per subject
        <input name="variants_per_subject" type="number" min={1} max={50} placeholder="4" value={variants} onChange={(e) => setVariants(e.target.value)} />
      </label>
    </Form>
  );
}

/* ------------------------------------------------------------------ connections */

const POSTGRES_FIELDS = ["host", "port", "user", "sslmode", "database", "schema", "token"];
const SNOWFLAKE_FIELDS = ["account", "warehouse", "role", "database", "schema", "token"];

/** Add a connection: the credential goes to the workspace, which never returns it; the field is
 * never filled back in. */
function AddConnection({ onClose }: { onClose: () => void }) {
  const scope = useWorkspace();
  const toast = useToast();
  const save = useAsync();
  const blank = { name: "", type: "postgres", host: "", port: "", user: "", sslmode: "require", account: "", warehouse: "", role: "", database: "", schema: "", token: "" };
  const [form, setForm] = useState<Record<string, string>>(blank);
  const set = (key: string) => (e: React.ChangeEvent<HTMLInputElement | HTMLSelectElement>) => setForm({ ...form, [key]: e.target.value });
  const postgres = form.type === "postgres";
  return (
    <Drawer
      title="Add a Connection"
      subtitle="Use a read-only account: only a single SELECT ever runs against it."
      onClose={onClose}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button variant="primary" type="submit" form="add-connection" loading={save.busy}>
            Save connection
          </Button>
        </>
      }
    >
      <Form
        id="add-connection"
        error={save.error}
        onSubmit={() =>
          save.run(async () => {
            const connector: Row = { type: form.type };
            for (const field of postgres ? POSTGRES_FIELDS : SNOWFLAKE_FIELDS) if (form[field]) connector[field] = form[field];
            await wa(scope, "create_connection", { name: form.name, connector });
            setForm(blank);
            toast(`Saved ${form.name}`);
            onClose();
          })
        }
      >
        <label>
          Name
          <input name="name" required pattern="[A-Za-z0-9_\-]{1,64}" placeholder="warehouse" value={form.name} onChange={set("name")} />
        </label>
        <label>
          Type
          <select name="type" value={form.type} onChange={set("type")}>
            <option value="postgres">Postgres</option>
            <option value="snowflake">Snowflake</option>
          </select>
        </label>
        {postgres ? (
          <>
            <label>
              Host
              <input name="host" placeholder="db.example.com" value={form.host} onChange={set("host")} />
            </label>
            <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
              <label>
                Port
                <input name="port" inputMode="numeric" placeholder="5432" value={form.port} onChange={set("port")} />
              </label>
              <label>
                SSL mode
                <select name="sslmode" value={form.sslmode} onChange={set("sslmode")}>
                  <option value="require">require</option>
                  <option value="verify-full">verify-full</option>
                  <option value="disable">disable</option>
                </select>
              </label>
            </div>
            <label>
              User
              <input name="user" value={form.user} onChange={set("user")} />
            </label>
          </>
        ) : (
          <>
            <label>
              Account
              <input name="account" placeholder="ab12345.us-east-1" value={form.account} onChange={set("account")} />
            </label>
            <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
              <label>
                Warehouse
                <input name="warehouse" value={form.warehouse} onChange={set("warehouse")} />
              </label>
              <label>
                Role
                <input name="role" value={form.role} onChange={set("role")} />
              </label>
            </div>
          </>
        )}
        <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
          <label>
            Database
            <input name="database" value={form.database} onChange={set("database")} />
          </label>
          <label>
            Schema
            <input name="schema" value={form.schema} onChange={set("schema")} />
          </label>
        </div>
        <label>
          Password or access token
          <input type="password" name="token" autoComplete="new-password" value={form.token} onChange={set("token")} />
          <small className="muted">Sent to the workspace, which never returns it.</small>
        </label>
      </Form>
    </Drawer>
  );
}

/* ------------------------------------------------------------------ storage */

function storePhase(store: Row): string {
  const phase = text(store, "phase");
  if (text(store, "desired") === "absent") return !phase || phase === "Ready" ? "Deleting" : phase;
  if (phase === "Ready" && store.connected === false) return "Connecting";
  return phase || "Waiting for the operator";
}

function phaseTone(phase: string): Tone {
  return phase === "Ready" ? "success" : phase === "Failed" ? "danger" : "warning";
}

/** "Volume" and "cluster disk" from the provider label "Volume — cluster disk". */
function storeKindWords(providers: Row[], type: string, provider: string): [string, string] {
  const known = providers.find((p) => text(p, "type") === type && text(p, "provider") === provider);
  if (!known) return [type, provider];
  const label = text(known, "label");
  const at = label.indexOf(" — ");
  return at < 0 ? [label, provider] : [label.slice(0, at), label.slice(at + 3)];
}

/** Data → Storage (docs/api/storage.md): shown whether or not the workspace is ready. */
function Storage() {
  const scope = useWorkspace();
  const toast = useToast();
  const stores = useStores();
  const providers = useSpaceRead<{ providers?: Row[] }>("list_store_providers", {}, { workspace: false, staleTime: 300_000 });
  const providerList = providers.data?.providers ?? [];
  const show = useAsync();
  const [connection, setConnection] = useState<Row | null>(null);
  const [creating, setCreating] = useState(false);
  const [deleting, setDeleting] = useState<{ id: string; name: string } | null>(null);
  const settling = (stores.data?.stores ?? []).some((s) => !["Ready", "Failed"].includes(storePhase(s)));
  return (
    <div className="mt-4" id="storage">
      <Card
        label="Storage"
        title="Buckets, Filesystems and Volumes"
        actions={
          <Button onClick={() => setCreating(true)}>
            <Plus size={14} aria-hidden /> New storage
          </Button>
        }
      >
        <p className="muted mt-0 text-[12.5px]">Storage for your services, private to this workspace.</p>
        {show.error ? <ErrorBlock error={show.error} /> : null}
        <Query q={stores} what="storage">
          {(value) =>
            (value.stores ?? []).length === 0 ? (
              <Empty compact icon={HardDrive} title="No storage yet" action={<Button onClick={() => setCreating(true)}>New storage</Button>}>
                Create a volume your services can mount.
              </Empty>
            ) : (
              <DataTable<Row>
                rows={value.stores ?? []}
                what="storage"
                rowKey={(s, i) => text(s, "id") || String(i)}
                columns={[
                  {
                    key: "name",
                    label: "Store",
                    sort: (s) => text(s, "name"),
                    render: (store) => {
                      const [type, provider] = storeKindWords(providerList, text(store, "type"), text(store, "provider"));
                      return (
                        <div className="min-w-0">
                          <div className="font-semibold text-ink">{text(store, "name")}</div>
                          <div className="text-[12px] text-ink-3">
                            {type} · {provider}
                          </div>
                        </div>
                      );
                    },
                  },
                  { key: "size", label: "Size", numeric: true, sort: (s) => Number(s.size_gi) || 0, render: (s) => <span className="font-mono">{text(s, "size_gi")} GiB</span> },
                  {
                    key: "status",
                    label: "Status",
                    sort: (s) => storePhase(s),
                    render: (store) => {
                      const phase = storePhase(store);
                      return (
                        <div className="min-w-0">
                          <Status value={phase} tone={phaseTone(phase)} />
                          {text(store, "message") && <div className="mt-0.5 text-[12px] text-ink-3">{text(store, "message")}</div>}
                        </div>
                      );
                    },
                  },
                  { key: "endpoint", label: "Endpoint", render: (s) => <Mono>{text(s, "endpoint")}</Mono> },
                  {
                    key: "actions",
                    label: "",
                    className: "text-right",
                    render: (store) => {
                      const id = text(store, "id");
                      const name = text(store, "name");
                      const phase = storePhase(store);
                      if (text(store, "desired") === "absent") return null;
                      return (
                        <div className="flex flex-wrap justify-end gap-1.5">
                          {phase === "Ready" && (
                            <Button
                              variant="ghost"
                              loading={show.busy}
                              onClick={() =>
                                void show.run(async () => {
                                  const answer = await act<Row>("store_connection", { customer_id: scope.customerId, workspace_id: scope.workspaceId, store_id: id });
                                  setConnection({ ...answer, name });
                                })
                              }
                            >
                              Show connection
                            </Button>
                          )}
                          <Button variant="ghost" onClick={() => setDeleting({ id, name })}>
                            Delete
                          </Button>
                        </div>
                      );
                    },
                  },
                ]}
              />
            )
          }
        </Query>
        {settling && <Banner tone="quiet">This list checks again every 3 seconds while storage is changing.</Banner>}
      </Card>
      {creating && <NewStorage providers={providerList} providersError={providers.isError ? providers.error : null} onClose={() => setCreating(false)} />}
      {deleting && <DeleteStore id={deleting.id} name={deleting.name} onClose={() => setDeleting(null)} onDeleted={() => toast(`Deleting ${deleting.name}`)} />}
      {connection && (
        <Modal title={`Connection to ${text(connection, "name")}`} subtitle="What a service in your workspace mounts or calls to use it. It is not shown again unless you ask." onClose={() => setConnection(null)}>
          <dl>
            {Object.entries(connection)
              .filter(([key]) => key !== "name")
              .map(([key, value]) => (
                <div key={key} className="contents">
                  <dt>{key}</dt>
                  <dd>
                    <code>{typeof value === "string" ? value : JSON.stringify(value)}</code>
                  </dd>
                </div>
              ))}
          </dl>
        </Modal>
      )}
    </div>
  );
}

/** Delete a store: the person types its name, which the server checks too. */
function DeleteStore({ id, name, onClose, onDeleted }: { id: string; name: string; onClose: () => void; onDeleted: () => void }) {
  const scope = useWorkspace();
  const remove = useAsync();
  const [confirm, setConfirm] = useState("");
  return (
    <Modal
      title={`Delete ${name}?`}
      onClose={onClose}
      width={460}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button variant="danger" type="submit" form="delete-store" loading={remove.busy} disabled={confirm !== name}>
            Delete {name}
          </Button>
        </>
      }
    >
      <Banner tone="warn">Deleting a store deletes the data in it, which cannot be recovered.</Banner>
      <Form
        id="delete-store"
        error={remove.error}
        onSubmit={() =>
          remove.run(async () => {
            await act("delete_store", { customer_id: scope.customerId, workspace_id: scope.workspaceId, store_id: id, confirm });
            setConfirm("");
            onDeleted();
            onClose();
          })
        }
      >
        <label>
          <span>
            Type <code>{name}</code> to delete it
          </span>
          <input name="confirm" required autoComplete="off" value={confirm} onChange={(e) => setConfirm(e.target.value)} />
        </label>
      </Form>
    </Modal>
  );
}

function NewStorage({ providers, providersError, onClose }: { providers: Row[]; providersError: unknown; onClose: () => void }) {
  const scope = useWorkspace();
  const toast = useToast();
  const create = useAsync();
  const available = providers.filter((p) => p.available);
  const more = providers.some((p) => !p.available);
  const [store, setStore] = useState("");
  const [name, setName] = useState("");
  const [size, setSize] = useState("10");
  const chosen = store || (available[0] ? `${text(available[0], "type")}:${text(available[0], "provider")}` : "");
  return (
    <Modal
      title="New Storage"
      onClose={onClose}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button variant="primary" type="submit" form="new-storage" loading={create.busy}>
            Create storage
          </Button>
        </>
      }
    >
      {providersError ? <ErrorBlock error={providersError} what="storage providers" /> : null}
      <Form
        id="new-storage"
        error={create.error}
        onSubmit={() =>
          create.run(async () => {
            const colon = chosen.indexOf(":");
            const input: Row = {
              customer_id: scope.customerId,
              workspace_id: scope.workspaceId,
              type: colon < 0 ? chosen : chosen.slice(0, colon),
              provider: colon < 0 ? "" : chosen.slice(colon + 1),
              name,
            };
            if (/^-?\d+$/.test(size)) input.size_gi = Number(size);
            await act("create_store", input);
            toast(`Creating ${name}`);
            setName("");
            onClose();
          })
        }
      >
        <Field label="Type and provider" help={more ? "Buckets (Cloudflare R2, AWS S3, Backblaze B2, Azure, Google Cloud) and shared filesystems are coming later." : undefined}>
          <select name="store" value={chosen} onChange={(e) => setStore(e.target.value)}>
            {available.map((p, i) => (
              <option key={i} value={`${text(p, "type")}:${text(p, "provider")}`}>
                {text(p, "label")}
              </option>
            ))}
          </select>
        </Field>
        <div className="grid grid-cols-1 gap-3 sm:grid-cols-[minmax(0,1fr)_8rem]">
          <Field label="Name">
            <input name="name" required pattern="[a-z][a-z0-9\-]{0,30}" placeholder="uploads" value={name} onChange={(e) => setName(e.target.value)} />
          </Field>
          <Field label="Size (GiB)">
            <input name="size_gi" type="number" min={1} max={100} value={size} onChange={(e) => setSize(e.target.value)} />
          </Field>
        </div>
      </Form>
    </Modal>
  );
}
