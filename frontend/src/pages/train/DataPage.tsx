/** Data → Datasets (web_ui.cpp workspace_content "data"): the workspace's datasets with figures and
 * a toolbar, and Load data (upload, generate, pull from a connection) in a drawer. Datasets only:
 * connections have their own page (Data → Connections, lake#651) and storage is a tab of Data →
 * Drives (lake#656); an old `?browse=<connection>`, `#connections` or `#storage` link is sent on.
 * Every form handler of /train/data/* is a capability call here. */
import { useEffect, useMemo, useState } from "react";
import { Link, useLocation, useNavigate, useSearchParams } from "react-router-dom";
import { Cable, Database, Sparkles, Table2, Terminal, Upload as UploadIcon } from "lucide-react";
import { Pull } from "@/pages/connections/forms";
import { CONNECTIONS_PATH, movedFromDatasets } from "@/pages/connections/connections";
import { type Row } from "@/lib/api";
import { bytes } from "@/lib/format";
import { Drawer } from "@/lib/overlay";
import { useToast } from "@/lib/toast";
import { useTrail } from "@/lib/trail";
import { Banner, Button, Chip, DataTable, Empty, FilePicker, FilterChips, Form, Query, SearchInput, Stat, StateTabs, StatRow, Toolbar } from "@/lib/ui";
import { WorkspaceFrame, qs, useAsync, useWorkspace, useWorkspaceReady, useWq, useWsPath, wa, text } from "./workspace";
import { annotateDatasets, resultDataset, useLeanDatasets } from "./datasets";
import { nameForFile, nextFreeExample } from "./upload";

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
      >
        {() => <DataContent onOpen={setOpen} />}
      </WorkspaceFrame>
      {open === "load" && <LoadData onClose={() => setOpen(null)} />}
    </>
  );
}

function DataContent({ onOpen }: { onOpen: (open: Open) => void }) {
  const [params] = useSearchParams();
  const datasets = useLeanDatasets();
  const loaded = params.get("loaded");

  const named = useMemo(() => annotateDatasets(datasets.data).filter((d) => text(d, "name")), [datasets.data]);
  const own = named.filter((d) => !resultDataset(d));
  const results = named.filter((d) => resultDataset(d));

  return (
    <>
      {loaded !== null && <LoadedNotice />}
      <StatRow cols={2}>
        <Stat label="Datasets" icon={Database} value={datasets.data ? own.length : "—"} sub="you can train on" />
        <Stat label="Run outputs" icon={Table2} value={datasets.data ? results.length : "—"} sub="tables runs wrote" />
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
  // The name Lake last filled in from a file, so choosing another file replaces it but never a
  // name the person typed (lake#814).
  const [filled, setFilled] = useState("");
  const [rename, setRename] = useState(false);
  const choose = (next: File | null) => {
    setFile(next);
    const name = nameForFile(tableName, filled, next?.name ?? null);
    if (name !== tableName) {
      setTableName(name);
      setFilled(name);
    }
  };
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
          onFile={choose}
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
        <input name="table_name" required pattern={NAME_PATTERN} placeholder="e.g. accounts" value={tableName} onChange={(e) => setTableName(e.target.value)} />
        <small className="muted">Named after the file; change it if you like. Letters, digits and underscores, starting with a letter or an underscore, so SQL can query it.</small>
      </label>
      <label className="flex! items-start gap-2">
        <input type="checkbox" name="if_exists" value="rename" checked={rename} onChange={(e) => setRename(e.target.checked)} />
        <span>If the name is taken, save it under the next free name ({nextFreeExample(tableName)})</span>
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
