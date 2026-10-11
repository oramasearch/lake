/** Data → Datasets (web_ui.cpp workspace_content "data", storage_content): the workspace's datasets
 * with figures and a toolbar, Load data (upload, generate, pull from a connection) in a drawer, and
 * its storage. Connections have their own page (Data → Connections, lake#651); an old
 * `?browse=<connection>` or `#connections` link is sent there. Every form handler of /train/data/*
 * is a capability call here. */
import { useEffect, useMemo, useState } from "react";
import { Link, useLocation, useNavigate, useSearchParams } from "react-router-dom";
import { Cable, Database, HardDrive, Plus, Sparkles, Table2, Terminal, Upload as UploadIcon } from "lucide-react";
import { Pull } from "@/pages/connections/forms";
import { CONNECTIONS_PATH, movedFromDatasets } from "@/pages/connections/connections";
import { act, type Row } from "@/lib/api";
import { bytes } from "@/lib/format";
import { Drawer, Modal } from "@/lib/overlay";
import { useToast } from "@/lib/toast";
import { useTrail } from "@/lib/trail";
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
import { WorkspaceFrame, qs, useAsync, useSpaceRead, useWorkspace, useWorkspaceReady, useWq, useWsPath, wa, text } from "./workspace";
import { annotateDatasets, resultDataset, useLeanDatasets } from "./datasets";

const SUB = "The datasets your workspace trains on.";
const NAME_PATTERN = "[A-Za-z_][A-Za-z0-9_]{0,62}";
const UPLOAD_LIMIT = 16 * 1024 * 1024;

type Open = null | "load";

export default function DataPage() {
  const [params] = useSearchParams();
  const location = useLocation();
  const navigate = useNavigate();
  const { customerId } = useWorkspace();
  const ws = useWsPath();
  const ready = useWorkspaceReady();
  const space = qs({ customer_id: customerId });
  const [open, setOpen] = useState<Open>(null);
  // Connections moved to their own page (lake#651): old links follow them there.
  const moved = movedFromDatasets(location.search, location.hash);
  useEffect(() => {
    if (moved) navigate(ws(moved), { replace: true });
  }, [moved, navigate, ws]);
  useTrail([
    { label: "Data", to: "/repositories" },
    { label: "Datasets", to: ws(`/train/data?${space}`) },
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
        {() => <DataContent onOpen={setOpen} />}
      </WorkspaceFrame>
      {open === "load" && <LoadData onClose={() => setOpen(null)} />}
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

function DataContent({ onOpen }: { onOpen: (open: Open) => void }) {
  const [params] = useSearchParams();
  const datasets = useLeanDatasets();
  const stores = useStores();
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
        <Stat label="Stores" icon={HardDrive} value={stores.data ? storeList.length : "—"} sub={changing ? `${changing} changing` : "buckets, filesystems, volumes"} />
      </StatRow>

      <DatasetList datasets={datasets} own={own} results={results} onLoad={() => onOpen("load")} />
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

/* ------------------------------------------------------------------ load data */

/** Load data: upload a file, generate a dataset, or pull a table from a connection. */
function LoadData({ onClose }: { onClose: () => void }) {
  const ws = useWsPath();
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
              <Empty compact icon={Cable} title="No connections yet" action={<Button to={ws(CONNECTIONS_PATH)}>Add one in Connections</Button>}>
                Connect the database your data lives in on the Connections page, then pull a table from it here.
              </Empty>
            ) : (
              <>
                <Pull connections={value.connections ?? []} onDone={onClose} onBusy={setBusy} />
                <p className="muted mt-2 mb-0 text-[12.5px]">
                  Pick one of this workspace's connections. To add another, or to browse a connection's tables first, open{" "}
                  <Link to={ws(CONNECTIONS_PATH)}>Connections</Link>.
                </p>
              </>
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
