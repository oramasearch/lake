/** Network (lake#729, docs/ui/network.md): the words, inputs and sentences the Network pages use.
 * Pure, so `tests/network.test.ts` runs it under Node. The rows are Lake's secure-networking
 * desired state and the operator's observation of it (`list_network_connections`,
 * `list_network_egress_rules`, `list_network_tunnel_grants`, `list_network_peers`, `network_rules`;
 * docs/api/network.md). A tunnel is named for what it reaches, never for what carries it. */

export type Row = Record<string, unknown>;

/** The section's pages, under `/w/<id>/`. `/network` itself opens the overview. */
export const NETWORK_PATH = "/network";
export const OVERVIEW_PATH = "/network/overview";
export const CONNECTIONS_PATH = "/network/connections";
export const ACCESS_PATH = "/network/access";
export const PEERS_PATH = "/network/private-networks";
export const EGRESS_PATH = "/network/egress";
export const POLICIES_PATH = "/network/policies";
export const ACTIVITY_PATH = "/network/activity";

/** Every page of the section, in the rail's order, with the capabilities it calls. The test checks
 * that together they reach every network capability (lake#728). */
export const NETWORK_PAGES: { path: string; label: string; capabilities: string[] }[] = [
  { path: OVERVIEW_PATH, label: "Overview", capabilities: ["network_rules", "list_network_tunnel_sessions", "network_activity"] },
  {
    path: CONNECTIONS_PATH,
    label: "Connections",
    capabilities: ["list_network_connections", "create_network_connection", "rotate_network_connection_credential", "remove_network_connection"],
  },
  {
    path: ACCESS_PATH,
    label: "Developer Access",
    capabilities: ["list_network_tunnel_grants", "grant_network_tunnel", "issue_network_tunnel_certificate", "revoke_network_tunnel", "list_network_tunnel_sessions"],
  },
  { path: PEERS_PATH, label: "Private Networks", capabilities: ["list_network_peers"] },
  { path: EGRESS_PATH, label: "Egress and DNS", capabilities: ["list_network_egress_rules", "create_network_egress_rule"] },
  { path: POLICIES_PATH, label: "Policies", capabilities: ["network_rules"] },
  { path: ACTIVITY_PATH, label: "Activity", capabilities: ["network_activity"] },
];

function str(row: Row | null | undefined, key: string): string {
  const value = row?.[key];
  return value === null || value === undefined ? "" : String(value);
}

function num(row: Row | null | undefined, key: string): number {
  const value = Number(row?.[key]);
  return Number.isFinite(value) ? value : 0;
}

export type Tone = "neutral" | "success" | "warning" | "danger" | "info";

/** A desired row's state in words: whether the cluster has caught up with what Lake asked for. */
export function recordState(record: Row): { label: string; tone: Tone; detail: string } {
  const status = record.status && typeof record.status === "object" ? (record.status as Row) : null;
  const absent = str(record, "desired") === "absent";
  const generation = num(record, "generation");
  if (!status) {
    return absent
      ? { label: "Removing", tone: "warning", detail: "Lake asked for its removal; the cluster has not reported yet." }
      : { label: "Waiting", tone: "warning", detail: "The cluster has not reported on it yet." };
  }
  const message = str(status, "message");
  const phase = str(status, "phase");
  if (num(status, "observed_generation") < generation) {
    return { label: absent ? "Removing" : "Updating", tone: "warning", detail: message || `The cluster last applied version ${num(status, "observed_generation")} of ${generation}.` };
  }
  if (absent) return { label: "Removed", tone: "neutral", detail: message || "The cluster removed it." };
  if (/^ready$/i.test(phase)) return { label: "Ready", tone: "success", detail: message || "The cluster applied it." };
  if (/^(failed|error|denied|rejected)$/i.test(phase)) return { label: "Failed", tone: "danger", detail: message || "The cluster could not apply it." };
  return { label: phase ? phase.charAt(0).toUpperCase() + phase.slice(1).toLowerCase() : "Waiting", tone: "warning", detail: message };
}

/** A tunnel grant's own state: revoked, expired or active, from its fields, not the cluster's. */
export function grantState(grant: Row, now = Date.now()): { label: "Active" | "Revoked" | "Expired"; tone: Tone } {
  if (grant.revoked_at) return { label: "Revoked", tone: "neutral" };
  const expires = Date.parse(str(grant, "expires_at"));
  if (grant.active === false || (Number.isFinite(expires) && expires <= now)) return { label: "Expired", tone: "neutral" };
  return { label: "Active", tone: "success" };
}

/** Whether the grant's recipient can ask for credentials now: active, and the relay ready at the
 * grant's current version with its endpoint and trust (docs/api/tunnel-grants.md). */
export function grantReady(grant: Row, now = Date.now()): boolean {
  if (grantState(grant, now).label !== "Active") return false;
  const status = grant.status && typeof grant.status === "object" ? (grant.status as Row) : null;
  return (
    !!status &&
    /^ready$/i.test(str(status, "phase")) &&
    num(status, "observed_generation") === num(grant, "generation") &&
    !!str(status, "endpoint") &&
    !!str(status, "relay_ca_pem")
  );
}

/** Ports in words: none listed means every port the target serves. */
export function portsText(ports: unknown): string {
  const list = Array.isArray(ports) ? ports : [];
  return list.length ? list.join(", ") : "Every port it serves";
}

/** A connection's credential scope in words. */
export function scopeText(scope: unknown): string {
  if (!scope || typeof scope !== "object") return "None";
  const s = scope as Row;
  const access = str(s, "access") === "write" ? "Read and write" : "Read only";
  const schema = str(s, "schema");
  return schema ? `${access}, schema ${schema}` : access;
}

/** Ports typed as "5432, 6432". Empty is allowed where the target's own ports apply. */
export function parsePorts(text: string, { required = false } = {}): number[] {
  const parts = text
    .split(/[\s,]+/)
    .map((p) => p.trim())
    .filter(Boolean);
  if (required && parts.length === 0) throw new Error("Name at least one port.");
  return parts.map((p) => {
    if (!/^\d+$/.test(p) || Number(p) < 1 || Number(p) > 65535) throw new Error("Ports are whole numbers from 1 to 65535, separated by commas.");
    return Number(p);
  });
}

export type ConnectionForm = {
  sourceKind: string;
  sourceId: string;
  targetKind: string;
  targetId: string;
  ports: string;
  access: "" | "read" | "write";
  schema: string;
};

/** `create_network_connection`'s input, beyond the account and workspace. A credential is asked for
 * only from an app to a service, the one pair the capability accepts it on. */
export function connectionInput(form: ConnectionForm): Row {
  if (!form.sourceId) throw new Error("Choose what connects.");
  if (!form.targetId) throw new Error("Choose what it connects to.");
  if (form.sourceKind === form.targetKind && form.sourceId === form.targetId) throw new Error("A connection joins two different things.");
  const input: Row = { source_kind: form.sourceKind, source_id: form.sourceId, target_kind: form.targetKind, target_id: form.targetId };
  const ports = parsePorts(form.ports);
  if (ports.length) input.ports = ports;
  if (form.access && credentialAllowed(form.sourceKind, form.targetKind)) {
    const scope: Row = { access: form.access };
    if (form.schema.trim()) scope.schema = form.schema.trim();
    input.credential_scope = scope;
  }
  return input;
}

export function credentialAllowed(sourceKind: string, targetKind: string): boolean {
  return sourceKind === "app" && targetKind === "service";
}

export type EgressForm = { appliesTo: "workspace" | "app" | "service" | "agent"; sourceId: string; destination: "dns" | "cidr"; value: string; ports: string };

const DNS_NAME = /^(\*\.)?([a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?\.)+[a-z]{2,63}\.?$/i;
const CIDR = /^(\d{1,3}(\.\d{1,3}){3}\/\d{1,2}|[0-9a-f:]+\/\d{1,3})$/i;

/** `create_network_egress_rule`'s input: exactly one destination, and a source only when the rule
 * is for one workload. */
export function egressInput(form: EgressForm): Row {
  const value = form.value.trim();
  if (!value) throw new Error(form.destination === "dns" ? "Name the host, such as api.example.com." : "Give an address range, such as 203.0.113.0/24.");
  if (form.destination === "dns" && !DNS_NAME.test(value)) throw new Error("That is not a host name, such as api.example.com.");
  if (form.destination === "cidr" && !CIDR.test(value)) throw new Error("That is not an address range, such as 203.0.113.0/24.");
  const input: Row = { ports: parsePorts(form.ports, { required: true }) };
  if (form.destination === "dns") input.dns_name = value.toLowerCase();
  else input.cidr = value;
  if (form.appliesTo !== "workspace") {
    if (!form.sourceId) throw new Error("Choose the workload the rule is for.");
    input.source_kind = form.appliesTo;
    input.source_id = form.sourceId;
  }
  return input;
}

/** An egress rule's destination as written. */
export function egressDestination(rule: Row): { kind: "DNS name" | "Address range"; value: string } {
  const dns = str(rule, "dns_name");
  return dns ? { kind: "DNS name", value: dns } : { kind: "Address range", value: str(rule, "cidr") };
}

/** How long a grant lasts, offered in the Grant access drawer. */
export const GRANT_DURATIONS: { hours: number; label: string }[] = [
  { hours: 1, label: "1 hour" },
  { hours: 8, label: "8 hours" },
  { hours: 24, label: "1 day" },
  { hours: 24 * 7, label: "7 days" },
  { hours: 24 * 30, label: "30 days" },
];

/** A UTC RFC 3339 instant, whole seconds: what `expires_at` takes. */
export function expiresAt(hours: number, now = Date.now()): string {
  return new Date(Math.floor((now + hours * 3_600_000) / 1000) * 1000).toISOString().replace(/\.\d{3}Z$/, "Z");
}

export type GrantForm = { userId: string; targetKind: string; targetId: string; port: string; hours: number };

/** `grant_network_tunnel`'s input. */
export function grantInput(form: GrantForm, now = Date.now()): Row {
  if (!form.userId) throw new Error("Choose who gets access.");
  if (!form.targetId) throw new Error("Choose what they reach.");
  const ports = parsePorts(form.port, { required: true });
  if (ports.length !== 1) throw new Error("A grant reaches one port.");
  return { user_id: form.userId, target_kind: form.targetKind, target_id: form.targetId, port: ports[0], expires_at: expiresAt(form.hours, now) };
}

/** The developer's key and request, made on their own machine: the key never reaches Lake. */
export const CSR_COMMAND = [
  "umask 077",
  "openssl req -new -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes \\",
  "  -keyout tunnel.key -out tunnel.csr -subj '/CN=developer'",
].join("\n");

/** A pasted request, checked before it is sent: one PEM certificate request within the limit. */
export function csrProblem(pem: string): string | null {
  const text = pem.trim();
  if (!text) return "Paste the contents of tunnel.csr.";
  if (text.length > 16384) return "The request is longer than 16 KiB.";
  if (!/^-----BEGIN (NEW )?CERTIFICATE REQUEST-----[\s\S]+-----END (NEW )?CERTIFICATE REQUEST-----$/.test(text)) return "That is not a certificate request: it starts with -----BEGIN CERTIFICATE REQUEST-----.";
  if (/PRIVATE KEY/.test(text)) return "That holds a private key. Paste only tunnel.csr; the key stays on your machine.";
  return null;
}

/** tunnel.crt: the certificate followed by its chain. */
export function certificateFile(result: Row): string {
  const cert = str(result, "certificate_pem").trimEnd();
  const chain = str(result, "chain_pem").trimEnd();
  return chain ? `${cert}\n${chain}\n` : `${cert}\n`;
}

/** Names for ids: what the pages show instead of a UUID. */
export type Names = { app: Map<string, string>; service: Map<string, string>; user: Map<string, string> };

export const KIND_WORDS: Record<string, string> = { app: "App", service: "Service", agent: "Agent", tunnel: "Tunnel" };

export function nameOf(names: Names, kind: string, id: string): string {
  const known = kind === "app" ? names.app.get(id) : kind === "service" ? names.service.get(id) : undefined;
  if (known) return known;
  if (kind === "tunnel") return "A developer tunnel";
  return `${(KIND_WORDS[kind] ?? kind).toLowerCase()} ${id.length > 9 ? id.slice(0, 8) + "…" : id}`;
}

/** A UTC instant as "2026-10-14 20:00 UTC", for sentences. */
export function utcText(value: unknown): string {
  const t = Date.parse(String(value ?? ""));
  if (!Number.isFinite(t)) return String(value ?? "");
  return new Date(t).toISOString().slice(0, 16).replace("T", " ") + " UTC";
}

/** One end of a path in a sentence: its kind, then its name ("app reporting"). */
function endText(names: Names, kind: string, id: string): string {
  const name = nameOf(names, kind, id);
  if (kind === "tunnel" || !(kind === "app" ? names.app.has(id) : kind === "service" ? names.service.has(id) : false)) return name;
  return `${(KIND_WORDS[kind] ?? kind).toLowerCase()} ${name}`;
}

/** The effective rules as sentences, as the research's Policies page reads (§26): what may reach
 * what, then that nothing else is asked for. Names are never recapitalized; a sentence starts with
 * the kind ("App reporting may reach service orders on TCP 5432."). */
export function ruleSentences(rules: Row, names: Names, now = Date.now()): string[] {
  const out: string[] = [];
  const list = (key: string) => (Array.isArray(rules[key]) ? (rules[key] as Row[]) : []);
  for (const c of list("connections")) {
    const from = endText(names, str(c, "source_kind"), str(c, "source_id"));
    const to = endText(names, str(c, "target_kind"), str(c, "target_id"));
    const ports = Array.isArray(c.ports) && c.ports.length ? ` on TCP ${(c.ports as unknown[]).join(", ")}` : "";
    const scope = c.credential_scope ? `, with its own ${scopeText(c.credential_scope).toLowerCase()} credential` : "";
    out.push(`${capitalize(from)} may reach ${to}${ports}${scope}.`);
  }
  for (const e of list("egress_rules")) {
    const who = str(e, "source_kind") ? capitalize(endText(names, str(e, "source_kind"), str(e, "source_id"))) : "Every workload in the workspace";
    const dest = egressDestination(e);
    out.push(`${who} may reach ${dest.value} on TCP ${portsText(e.ports)}.`);
  }
  for (const g of list("tunnel_grants")) {
    if (grantState(g, now).label !== "Active") continue;
    const who = names.user.get(str(g, "user_id")) ?? "A member";
    out.push(`${who} may open a tunnel to ${endText(names, str(g, "target_kind"), str(g, "target_id"))} on TCP ${str(g, "port")} until ${utcText(g.expires_at)}.`);
  }
  for (const p of list("peers")) {
    out.push(`The private network ${str(p, "name")} is joined to this workspace.`);
  }
  out.push(out.length ? "Lake asks for no other path into or out of this workspace." : "Lake asks for no path into or out of this workspace.");
  return out;
}

function capitalize(text: string): string {
  return text ? text.charAt(0).toUpperCase() + text.slice(1) : text;
}

/** Only the rows still asked for, unless the history is wanted. */
export function present<T extends Row>(rows: T[], history: boolean): T[] {
  return history ? rows : rows.filter((r) => str(r, "desired") !== "absent");
}

/** A provider's record page (sessions, activity): whether Lake records them yet. */
export function recordsAvailable(page: Row | undefined): boolean {
  return page?.available === true;
}

/** One activity or session record's columns. The records are the workspace's own and their shape
 * is the provider's (zetta#1281), so the page reads the common names and shows the rest as is. */
export function recordColumns(record: Row): { when: string; source: string; destination: string; outcome: string } {
  const pick = (...keys: string[]) => {
    for (const k of keys) if (str(record, k)) return str(record, k);
    return "";
  };
  return {
    when: pick("at", "time", "timestamp", "started_at", "created_at"),
    source: pick("source", "source_id", "user_id", "from"),
    destination: pick("destination", "target", "target_id", "to"),
    outcome: pick("decision", "verdict", "outcome", "state", "status"),
  };
}
