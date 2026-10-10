/** Train → Data (web_ui.cpp workspace_content "data", storage_content): load data, the workspace's
 * connections and datasets, and the space's storage. Every form handler of /train/data/* is a
 * capability call here. */
import { useState } from "react";
import { Link, useNavigate, useSearchParams } from "react-router-dom";
import { act, type Row } from "@/lib/api";
import { Modal } from "@/lib/overlay";
import { useToast } from "@/lib/toast";
import { useTrail, entityCrumb } from "@/lib/trail";
import { Button, ErrorBlock, Form, Query } from "@/lib/ui";
import { WorkspaceFrame, qs, useAsync, useSpaceRead, useWorkspace, useWq, useWsPath, wa, text } from "./workspace";
import { annotateDatasets, resultDataset, useLeanDatasets } from "./datasets";

const SUB = "The connections and datasets your workspace trains on.";
const NAME_PATTERN = "[A-Za-z_][A-Za-z0-9_]{0,62}";
const UPLOAD_LIMIT = 16 * 1024 * 1024;

export default function DataPage() {
  const [params] = useSearchParams();
  const browse = params.get("browse") ?? "";
  const { customerId } = useWorkspace();
  const ws = useWsPath();
  const space = qs({ customer_id: customerId });
  useTrail([
    { label: "Train", to: ws(`/train/data?${space}`) },
    { label: "Data", to: ws(`/train/data?${space}`) },
    ...(browse ? [entityCrumb(browse)] : []),
  ]);
  const error = params.get("error");
  return (
    <WorkspaceFrame
      eyebrow="Train"
      title={browse ? `Data: ${browse}` : "Data"}
      sub={SUB}
      before={
        error ? (
          <p className="error" role="alert">
            {error}
          </p>
        ) : null
      }
      always={<Storage />}
    >
      {() => <DataContent browse={browse} />}
    </WorkspaceFrame>
  );
}

function DataContent({ browse }: { browse: string }) {
  const [params] = useSearchParams();
  const { customerId } = useWorkspace();
  const ws = useWsPath();
  const space = qs({ customer_id: customerId });
  const connections = useWq<{ connections?: Row[] }>("list_connections");
  const datasets = useLeanDatasets();
  const generators = useWq<{ generators?: Row[] }>("list_dataset_generators");
  const services = useSpaceRead<{ services?: Row[] }>("list_services", {}, { workspace: false });
  const tables = useWq<{ tables?: Row[] }>(browse ? "list_source_tables" : null, { connection: browse, include_columns: true, max_tables: 100 });
  const connectionList = connections.data?.connections ?? [];
  const generatorList = generators.data?.generators ?? [];
  const serviceList = services.data?.services ?? [];
  const loaded = params.get("loaded");

  return (
    <>
      {loaded !== null && <LoadedNotice />}
      <section className="panel" id="load-data">
        <h2>Load Data</h2>
        <p className="sub">
          Bring data into your workspace: upload a file, pull a table from a database you connected below, or generate one. Each becomes a dataset
          you can train on. <Link to="/docs/load-data">How loading data works</Link>
        </p>
        <Upload />
        {connectionList.length > 0 && <Pull connections={connectionList} />}
        {generatorList.length > 0 && <Generate generators={generatorList} />}
        {generators.isError ? <ErrorBlock error={generators.error} what="dataset generators" retry={() => void generators.refetch()} /> : null}
      </section>
      <section className="panel">
        <h2>Connections</h2>
        <Query q={connections} what="connections">
          {(value) =>
            (value.connections ?? []).length === 0 ? (
              <p>No connections yet. Add the database your data lives in below.</p>
            ) : (
              <ConnectionsTable connections={value.connections ?? []} services={serviceList} />
            )
          }
        </Query>
        {browse && (
          <>
            <h3>Tables in {browse}</h3>
            <Query q={tables} what="tables">
              {(value) =>
                (value.tables ?? []).length === 0 ? (
                  <p>No tables.</p>
                ) : (
                  <>
                    <div className="table-wrap">
                      <table>
                        <thead>
                          <tr>
                            <th>Schema</th>
                            <th>Table</th>
                            <th>Kind</th>
                            <th>Columns</th>
                          </tr>
                        </thead>
                        <tbody>
                          {(value.tables ?? []).map((table, i) => (
                            <tr key={i}>
                              <td>{text(table, "schema")}</td>
                              <td>{text(table, "table")}</td>
                              <td>{text(table, "kind")}</td>
                              <td className="muted">
                                {((table.columns ?? []) as Row[]).map((c) => `${text(c, "column")} ${text(c, "type")}`).join(", ")}
                              </td>
                            </tr>
                          ))}
                        </tbody>
                      </table>
                    </div>
                    <p className="muted">
                      Pull one with <em>Pull a dataset</em> below.
                    </p>
                  </>
                )
              }
            </Query>
          </>
        )}
        <Query q={services} what="services" lines={1}>
          {(value) => <ConnectServices services={value.services ?? []} connections={connectionList} />}
        </Query>
        <AddConnection />
      </section>
      <section className="panel">
        <h2>Datasets</h2>
        <Query q={datasets} what="datasets">
          {(value) => {
            const named = annotateDatasets(value).filter((d) => text(d, "name"));
            const own = named.filter((d) => !resultDataset(d));
            const results = named.filter((d) => resultDataset(d));
            return (
              <>
                {own.length === 0 ? <p>No datasets yet.</p> : <DatasetTable rows={own} />}
                {results.length > 0 && (
                  <details>
                    <summary>Results ({results.length})</summary>
                    <p className="muted">Tables the engine wrote as the output of runs. They are not offered as training data.</p>
                    <DatasetTable rows={results} />
                  </details>
                )}
              </>
            );
          }}
        </Query>
        <p className="muted">Open a dataset for its columns and their types, row count, size and first rows.</p>
        <p>
          <Link to={ws(`/train/query?${space}`)}>Query your data with SQL</Link>
        </p>
      </section>
    </>
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
    <section className="panel notice" role="status">
      <p>
        Loaded <strong>{params.get("rows") ?? ""}</strong> rows {format === "csv" ? "from your CSV file " : format === "jsonl" ? "from your JSONL file " : ""}
        into <Link to={ws(`/train/dataset?${qs({ customer_id: customerId, database, name: table })}`)}>{table}</Link>.
        {renamed && renamed !== table && (
          <>
            {" "}
            <strong>{renamed}</strong> was already taken, so it was saved as <strong>{table}</strong>.
          </>
        )}
      </p>
    </section>
  );
}

function DatasetTable({ rows }: { rows: Row[] }) {
  const { customerId } = useWorkspace();
  const ws = useWsPath();
  return (
    <div className="table-wrap">
      <table>
        <thead>
          <tr>
            <th>Name</th>
            <th>Database</th>
          </tr>
        </thead>
        <tbody>
          {rows.map((dataset, i) => {
            const database = text(dataset, "database") || "datasets";
            const name = text(dataset, "name");
            return (
              <tr key={i}>
                <td>
                  <Link to={ws(`/train/dataset?${qs({ customer_id: customerId, database, name })}`)}>{name}</Link>
                </td>
                <td>{database}</td>
              </tr>
            );
          })}
        </tbody>
      </table>
    </div>
  );
}

function ConnectionsTable({ connections, services }: { connections: Row[]; services: Row[] }) {
  const scope = useWorkspace();
  const ws = useWsPath();
  const toast = useToast();
  const remove = useAsync();
  const space = qs({ customer_id: scope.customerId });
  return (
    <>
      {remove.error ? (
        <p className="error" role="alert">
          {(remove.error as Error).message}
        </p>
      ) : null}
      <div className="table-wrap">
        <table>
          <thead>
            <tr>
              <th>Name</th>
              <th>Type</th>
              <th>Where</th>
              <th>From service</th>
              <th>Saved</th>
              <th></th>
            </tr>
          </thead>
          <tbody>
            {connections.map((connection, i) => {
              const connector = (connection.connector ?? {}) as Row;
              const name = text(connection, "name");
              const where = text(connector, "type") === "postgres" ? `${text(connector, "host")}/${text(connector, "database")}` : text(connector, "account");
              const host = text(connector, "host");
              const service = services.find((s) => text(s, "name") === name && host && text(s, "endpoint").startsWith(host));
              return (
                <tr key={i}>
                  <td>{name}</td>
                  <td>{text(connector, "type")}</td>
                  <td>
                    {where} · <Link to={ws(`/train/data?${qs({ customer_id: scope.customerId, browse: name })}`)}>Browse tables</Link>
                  </td>
                  <td>{service ? <Link to={`/services?${space}`}>{text(service, "name")}</Link> : "—"}</td>
                  <td>{text(connection, "created")}</td>
                  <td>
                    <Button
                      loading={remove.busy}
                      onClick={() =>
                        void remove.run(async () => {
                          await wa(scope, "delete_connection", { name });
                          toast(`Removed ${name}`);
                        })
                      }
                    >
                      Remove
                    </Button>
                  </td>
                </tr>
              );
            })}
          </tbody>
        </table>
      </div>
    </>
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
    <>
      <h3>Connect One of Your Services</h3>
      <p className="muted">The workspace reaches it inside the cluster, with the credential lake already holds; nothing to type.</p>
      {connect.error ? (
        <p className="error" role="alert">
          {(connect.error as Error).message}
        </p>
      ) : null}
      <div className="table-wrap">
        <table>
          <thead>
            <tr>
              <th>Service</th>
              <th>Kind</th>
              <th></th>
            </tr>
          </thead>
          <tbody>
            {rows.map((service, i) => {
              const kind = text(service, "kind");
              const name = text(service, "name");
              const ready = text(service, "phase") === "Ready" && service.connected !== false;
              return (
                <tr key={i}>
                  <td>{name}</td>
                  <td>{kind}</td>
                  <td>
                    {kind !== "postgres" ? (
                      <span className="muted">The workspace can't read {kind} yet; Postgres works today</span>
                    ) : !ready ? (
                      <span className="muted">starting</span>
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
                  </td>
                </tr>
              );
            })}
          </tbody>
        </table>
      </div>
    </>
  );
}

const POSTGRES_FIELDS = ["host", "port", "user", "sslmode", "database", "schema", "token"];
const SNOWFLAKE_FIELDS = ["account", "warehouse", "role", "database", "schema", "token"];

/** Add a connection: the credential goes to the workspace, which never returns it; the field is
 * never filled back in. */
function AddConnection() {
  const scope = useWorkspace();
  const toast = useToast();
  const save = useAsync();
  const blank = { name: "", type: "postgres", host: "", port: "", user: "", sslmode: "require", account: "", warehouse: "", role: "", database: "", schema: "", token: "" };
  const [form, setForm] = useState<Record<string, string>>(blank);
  const set = (key: string) => (e: React.ChangeEvent<HTMLInputElement | HTMLSelectElement>) => setForm({ ...form, [key]: e.target.value });
  return (
    <details>
      <summary>Add a connection</summary>
      <Form
        error={save.error}
        onSubmit={() =>
          save.run(async () => {
            const connector: Row = { type: form.type };
            for (const field of form.type === "postgres" ? POSTGRES_FIELDS : SNOWFLAKE_FIELDS) if (form[field]) connector[field] = form[field];
            await wa(scope, "create_connection", { name: form.name, connector });
            setForm(blank);
            toast(`Saved ${form.name}`);
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
          <fieldset>
            <legend>Postgres</legend>
            <label>
              Host
              <input name="host" placeholder="db.example.com" value={form.host} onChange={set("host")} />
            </label>
            <label>
              Port
              <input name="port" inputMode="numeric" placeholder="5432" value={form.port} onChange={set("port")} />
            </label>
            <label>
              User
              <input name="user" value={form.user} onChange={set("user")} />
            </label>
            <label>
              SSL mode
              <select name="sslmode" value={form.sslmode} onChange={set("sslmode")}>
                <option value="require">require</option>
                <option value="verify-full">verify-full</option>
                <option value="disable">disable</option>
              </select>
            </label>
          </fieldset>
          <fieldset>
            <legend>Snowflake</legend>
            <label>
              Account
              <input name="account" placeholder="ab12345.us-east-1" value={form.account} onChange={set("account")} />
            </label>
            <label>
              Warehouse
              <input name="warehouse" value={form.warehouse} onChange={set("warehouse")} />
            </label>
            <label>
              Role
              <input name="role" value={form.role} onChange={set("role")} />
            </label>
          </fieldset>
          <label>
            Database
            <input name="database" value={form.database} onChange={set("database")} />
          </label>
          <label>
            Schema
            <input name="schema" value={form.schema} onChange={set("schema")} />
          </label>
          <label>
            Password or access token
            <input type="password" name="token" autoComplete="new-password" value={form.token} onChange={set("token")} />
          </label>
        <p className="muted">Use a read-only account: only a single SELECT ever runs against it.</p>
        <Button variant="primary" type="submit" loading={save.busy}>
          Save connection
        </Button>
      </Form>
    </details>
  );
}

function Upload() {
  const scope = useWorkspace();
  const navigate = useNavigate();
  const ws = useWsPath();
  const upload = useAsync();
  const [file, setFile] = useState<File | null>(null);
  const [format, setFormat] = useState("");
  const [tableName, setTableName] = useState("");
  const [rename, setRename] = useState(false);
  return (
    <details open>
      <summary>Upload a file</summary>
      <Form
        error={upload.error}
        onSubmit={() =>
          upload.run(async () => {
            if (!file || file.size === 0) throw new Error("Choose one CSV or JSONL file.");
            if (file.size > UPLOAD_LIMIT) throw new Error("The file is larger than 16 MiB.");
            const chosen = format || (file.name.endsWith(".jsonl") || file.name.endsWith(".ndjson") ? "jsonl" : "csv");
            const content = await file.text();
            const result = await wa<Row>(scope, "upload_dataset", { table_name: tableName, format: chosen, content, if_exists: rename ? "rename" : "fail" });
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
        <label>
          File (CSV or JSONL, up to 16 MiB)
          <input type="file" name="file" required accept=".csv,.jsonl,.ndjson,text/csv" onChange={(e) => setFile(e.target.files?.[0] ?? null)} />
        </label>
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
          <small>Letters, digits and underscores, starting with a letter, so SQL can query it.</small>
        </label>
        <label>
          <input type="checkbox" name="if_exists" value="rename" checked={rename} onChange={(e) => setRename(e.target.checked)} /> If the name is taken, save
          it under the next free name (accounts_2)
        </label>
        <p className="muted">
          Otherwise a taken name is refused, and a CSV row whose field count differs from the header is refused with its line number.
        </p>
        <Button variant="primary" type="submit" loading={upload.busy}>
          Upload
        </Button>
      </Form>
    </details>
  );
}

function Pull({ connections }: { connections: Row[] }) {
  const scope = useWorkspace();
  const toast = useToast();
  const pull = useAsync();
  const [connection, setConnection] = useState(text(connections[0], "name"));
  const [query, setQuery] = useState("");
  const [tableName, setTableName] = useState("");
  const [maxRows, setMaxRows] = useState("");
  return (
    <details>
      <summary>Pull a table from a connection</summary>
      <Form
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
          <textarea name="query" rows={4} required placeholder="SELECT account_id, plan, seats, renewed FROM accounts" value={query} onChange={(e) => setQuery(e.target.value)} />
        </label>
        <label>
          Dataset name
          <input name="table_name" required pattern={NAME_PATTERN} placeholder="accounts" value={tableName} onChange={(e) => setTableName(e.target.value)} />
        </label>
        <label>
          At most this many rows
          <input name="max_rows" inputMode="numeric" placeholder="100000" value={maxRows} onChange={(e) => setMaxRows(e.target.value)} />
        </label>
        <Button variant="primary" type="submit" loading={pull.busy}>
          Pull dataset
        </Button>
      </Form>
    </details>
  );
}

function Generate({ generators }: { generators: Row[] }) {
  const scope = useWorkspace();
  const toast = useToast();
  const generate = useAsync();
  const [kind, setKind] = useState(text(generators[0], "kind"));
  const [variants, setVariants] = useState("");
  return (
    <details>
      <summary>Generate a dataset</summary>
      <Form
        error={generate.error}
        onSubmit={() =>
          generate.run(async () => {
            // Never `path`: it would write a file on the workspace's node.
            const input: Row = { kind };
            if (variants) input.variants_per_subject = Math.min(50, Math.max(1, parseInt(variants, 10) || 0));
            await wa(scope, "generate_dataset", input);
            toast("Dataset generated");
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
        <Button variant="primary" type="submit" loading={generate.busy}>
          Generate
        </Button>
      </Form>
    </details>
  );
}

/* ------------------------------------------------------------------ storage */

function storePhase(store: Row): string {
  const phase = text(store, "phase");
  if (text(store, "desired") === "absent") return !phase || phase === "Ready" ? "Deleting" : phase;
  if (phase === "Ready" && store.connected === false) return "Connecting";
  return phase || "Waiting for the operator";
}

/** "Volume" and "cluster disk" from the provider label "Volume — cluster disk". */
function storeKindWords(providers: Row[], type: string, provider: string): [string, string] {
  const known = providers.find((p) => text(p, "type") === type && text(p, "provider") === provider);
  if (!known) return [type, provider];
  const label = text(known, "label");
  const at = label.indexOf(" — ");
  return at < 0 ? [label, provider] : [label.slice(0, at), label.slice(at + 3)];
}

/** Train → Data → Storage (docs/api/storage.md): shown whether or not the workspace is ready. */
function Storage() {
  const scope = useWorkspace();
  const toast = useToast();
  const stores = useSpaceRead<{ stores?: Row[] }>("list_stores", {}, {
    refetchInterval: (query) => {
      const list = ((query.state.data as { stores?: Row[] } | undefined)?.stores ?? []) as Row[];
      return list.some((s) => !["Ready", "Failed"].includes(storePhase(s))) ? 3_000 : false;
    },
  });
  const providers = useSpaceRead<{ providers?: Row[] }>("list_store_providers", {}, { workspace: false, staleTime: 300_000 });
  const providerList = providers.data?.providers ?? [];
  const show = useAsync();
  const [connection, setConnection] = useState<Row | null>(null);
  const settling = (stores.data?.stores ?? []).some((s) => !["Ready", "Failed"].includes(storePhase(s)));
  return (
    <section className="panel" id="storage">
      <h2>Storage</h2>
      <p className="muted">Buckets, filesystems and volumes for your services. Private to your space.</p>
      {show.error ? (
        <p className="error" role="alert">
          {(show.error as Error).message}
        </p>
      ) : null}
      <Query q={stores} what="storage">
        {(value) =>
          (value.stores ?? []).length === 0 ? (
            <p>No storage yet.</p>
          ) : (
            <div className="table-wrap">
              <table>
                <thead>
                  <tr>
                    <th>Name</th>
                    <th>Type</th>
                    <th>Provider</th>
                    <th>Size</th>
                    <th>Status</th>
                    <th>Endpoint</th>
                    <th></th>
                  </tr>
                </thead>
                <tbody>
                  {(value.stores ?? []).map((store, i) => {
                    const id = text(store, "id");
                    const name = text(store, "name");
                    const phase = storePhase(store);
                    const deleting = text(store, "desired") === "absent";
                    const [type, provider] = storeKindWords(providerList, text(store, "type"), text(store, "provider"));
                    return (
                      <tr key={id || i}>
                        <td>{name}</td>
                        <td>{type}</td>
                        <td>{provider}</td>
                        <td>{text(store, "size_gi")} GiB</td>
                        <td>
                          {phase}
                          {text(store, "message") && (
                            <>
                              <br />
                              <span className="muted">{text(store, "message")}</span>
                            </>
                          )}
                        </td>
                        <td>
                          <code>{text(store, "endpoint")}</code>
                        </td>
                        <td>
                          {!deleting && phase === "Ready" && (
                            <Button
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
                          {!deleting && <DeleteStore id={id} name={name} onDeleted={() => toast(`Deleting ${name}`)} />}
                        </td>
                      </tr>
                    );
                  })}
                </tbody>
              </table>
            </div>
          )
        }
      </Query>
      {settling && <p className="muted">This page checks again every 3 seconds while storage is changing.</p>}
      <NewStorage providers={providerList} providersError={providers.isError ? providers.error : null} />
      {connection && (
        <Modal title={`Connection to ${text(connection, "name")}`} onClose={() => setConnection(null)}>
          <p>What a service in your space mounts or calls to use it. It is not shown again unless you ask.</p>
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
    </section>
  );
}

function DeleteStore({ id, name, onDeleted }: { id: string; name: string; onDeleted: () => void }) {
  const scope = useWorkspace();
  const remove = useAsync();
  const [confirm, setConfirm] = useState("");
  return (
    <details>
      <summary>Delete</summary>
      <Form
        error={remove.error}
        onSubmit={() =>
          remove.run(async () => {
            await act("delete_store", { customer_id: scope.customerId, workspace_id: scope.workspaceId, store_id: id, confirm });
            setConfirm("");
            onDeleted();
          })
        }
      >
        <p className="notice status-warning">Deleting a store deletes the data in it, which cannot be recovered.</p>
        <label>
          Type <code>{name}</code> to delete it
          <input name="confirm" required autoComplete="off" value={confirm} onChange={(e) => setConfirm(e.target.value)} />
        </label>
        <Button variant="danger" type="submit" loading={remove.busy}>
          Delete {name}
        </Button>
      </Form>
    </details>
  );
}

function NewStorage({ providers, providersError }: { providers: Row[]; providersError: unknown }) {
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
    <>
      <h3>New Storage</h3>
      {providersError ? (
        <p className="error" role="alert">
          {(providersError as Error).message}
        </p>
      ) : null}
      <Form
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
          })
        }
      >
        <label>
          Type and provider
          <select name="store" value={chosen} onChange={(e) => setStore(e.target.value)}>
            {available.map((p, i) => (
              <option key={i} value={`${text(p, "type")}:${text(p, "provider")}`}>
                {text(p, "label")}
              </option>
            ))}
          </select>
          {more && <small>Buckets (Cloudflare R2, AWS S3, Backblaze B2, Azure, Google Cloud) and shared filesystems are coming later.</small>}
        </label>
        <label>
          Name
          <input name="name" required pattern="[a-z][a-z0-9\-]{0,30}" placeholder="uploads" value={name} onChange={(e) => setName(e.target.value)} />
        </label>
        <label>
          Size (GiB)
          <input name="size_gi" type="number" min={1} max={100} value={size} onChange={(e) => setSize(e.target.value)} />
        </label>
        <Button variant="primary" type="submit" loading={create.busy}>
          Create storage
        </Button>
      </Form>
    </>
  );
}
