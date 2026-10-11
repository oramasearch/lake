/** Data → Connections (lake#651, docs/ui/connections.md): the words and inputs the Connections pages
 * use, and where the old Datasets links go now. Pure, so `tests/connections.test.ts` runs it under
 * Node. A connection is the workspace engine's (`list_connections`, `create_connection`,
 * `delete_connection`, `list_source_tables`, reached through `workspace_query`/`workspace_act`). */

export type Row = Record<string, unknown>;

/** The list page and one connection's page, under `/w/<id>/`. */
export const CONNECTIONS_PATH = "/connections";
export const CONNECTION_PATH = "/connections/connection";

export function connectionPath(name: string): string {
  return `${CONNECTION_PATH}?name=${encodeURIComponent(name)}`;
}

function str(row: Row | null | undefined, key: string): string {
  const value = row?.[key];
  return value === null || value === undefined ? "" : String(value);
}

function connectorOf(connection: Row): Row {
  const connector = connection.connector;
  return connector && typeof connector === "object" ? (connector as Row) : {};
}

/** The connection's kind (`postgres`, `snowflake`). */
export function connectionKind(connection: Row): string {
  return str(connectorOf(connection), "type");
}

export const KIND_LABELS: Record<string, string> = { postgres: "Postgres", snowflake: "Snowflake" };

export function kindLabel(kind: string): string {
  return KIND_LABELS[kind] ?? (kind || "Unknown");
}

/** Where a connection points: `host:port/database` for Postgres, the account for Snowflake. */
export function connectionHost(connection: Row): string {
  const connector = connectorOf(connection);
  if (connectionKind(connection) === "snowflake") {
    const account = str(connector, "account");
    const database = str(connector, "database");
    return account && database ? `${account}/${database}` : account;
  }
  const host = str(connector, "host");
  const port = str(connector, "port");
  const database = str(connector, "database");
  const at = host && port ? `${host}:${port}` : host;
  return at && database ? `${at}/${database}` : at || database;
}

export type ConnectionStatus = { label: string; tone: "success" | "warning" | "danger" | "neutral"; detail: string };

/** The connection's state, from what the engine says of its credential (`credential`: `set`,
 * `none`, `unreadable` or `plaintext`). The engine never returns the credential itself. */
export function connectionStatus(connection: Row): ConnectionStatus {
  switch (str(connection, "credential")) {
    case "set":
      return { label: "Ready", tone: "success", detail: "Its credential is sealed in the workspace." };
    case "none":
      return { label: "No credential", tone: "neutral", detail: "Saved without a password or token." };
    case "unreadable":
      return { label: "Credential unreadable", tone: "danger", detail: "The workspace cannot open its credential; add the connection again." };
    case "plaintext":
      return { label: "Not sealed", tone: "warning", detail: "Saved before credentials were sealed; add it again to seal it." };
    default:
      return { label: "Saved", tone: "neutral", detail: "" };
  }
}

/** The service a connection was made from (Connect One of Your Services names the connection after
 * the service and points it at the service's endpoint). */
export function serviceFor<T extends Row>(connection: Row, services: T[]): T | undefined {
  const name = str(connection, "name");
  const host = str(connectorOf(connection), "host");
  if (!host) return undefined;
  return services.find((s) => str(s, "name") === name && str(s, "endpoint").startsWith(host));
}

/** The connections that use a service, for Ship → Services and its delete warning. */
export function connectionsUsing(service: Row, connections: Row[]): Row[] {
  return connections.filter((c) => serviceFor(c, [service]) !== undefined);
}

/** The "Tables reachable" figure: the sum of each connection's tables, `+` when one listing was
 * cut short, `—` until every connection has answered, and the number of connections that failed. */
export function tablesFigure(answers: ({ tables?: unknown[]; truncated?: boolean } | undefined | "failed")[]): { value: string; failed: number } {
  let total = 0;
  let cut = false;
  let failed = 0;
  for (const answer of answers) {
    if (answer === undefined) return { value: "—", failed };
    if (answer === "failed") {
      failed += 1;
      continue;
    }
    total += (answer.tables ?? []).length;
    cut ||= !!answer.truncated;
  }
  if (failed === answers.length && answers.length > 0) return { value: "—", failed };
  return { value: `${total.toLocaleString("en-US")}${cut ? "+" : ""}`, failed };
}

export const POSTGRES_FIELDS = ["host", "port", "user", "sslmode", "database", "schema", "token"] as const;
export const SNOWFLAKE_FIELDS = ["account", "warehouse", "role", "database", "schema", "token"] as const;

export const CONNECTION_NAME = /^[A-Za-z0-9_-]{1,64}$/;

/** The `create_connection` input from the Add connection form: only the kind's fields, only those
 * filled in. The token goes to the workspace, which never returns it. */
export function connectionInput(form: Record<string, string>): { name: string; connector: Row } {
  const type = form.type === "snowflake" ? "snowflake" : "postgres";
  const connector: Row = { type };
  for (const field of type === "postgres" ? POSTGRES_FIELDS : SNOWFLAKE_FIELDS) if (form[field]) connector[field] = form[field];
  return { name: (form.name ?? "").trim(), connector };
}

/** Delete is offered only once the person has typed the connection's name exactly. */
export function deleteConfirmed(name: string, typed: string): boolean {
  return !!name && typed === name;
}

/** A query that pulls one source table whole, for "Load into a dataset". */
export function tableQuery(schema: string, table: string, kind: string): string {
  const quote = (part: string) => (/^[a-z_][a-z0-9_]*$/.test(part) ? part : `"${part.replace(/"/g, '""')}"`);
  const from = schema ? `${quote(schema)}.${quote(table)}` : quote(table);
  return kind === "snowflake" ? `SELECT * FROM ${schema ? `${schema}.${table}` : table}` : `SELECT * FROM ${from}`;
}

/** A dataset name from a table's name: letters, digits and underscores, starting with a letter. */
export function datasetNameFor(table: string): string {
  const cleaned = table.replace(/[^A-Za-z0-9_]/g, "_").replace(/^[^A-Za-z_]+/, "");
  return (cleaned || "dataset").slice(0, 63);
}

/** Where an old Data → Datasets link goes now (lake#651): `?browse=<connection>` opens that
 * connection, `#connections` the Connections page. Null when the link is a Datasets link still.
 * The answer is workspace-local; the page prefixes `/w/<id>`. */
export function movedFromDatasets(search: string, hash: string): string | null {
  const params = new URLSearchParams(search);
  const browse = params.get("browse");
  if (browse) return connectionPath(browse);
  const anchor = hash.replace(/^#/, "");
  if (anchor === "connections" || anchor === "connection") return CONNECTIONS_PATH;
  return null;
}
