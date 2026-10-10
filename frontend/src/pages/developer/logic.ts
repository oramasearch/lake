/** Developer → API and Developer → MCP, the parts that are rules rather than layout: the
 * `create_token` input a key form builds, how a token's scopes read, and the OpenAPI document
 * turned into searchable groups of operations with a curl line each. No imports from the app, so
 * `tests/developer.test.ts` runs it directly. */

// eslint-disable-next-line @typescript-eslint/no-explicit-any
type Row = Record<string, any>;

/* ------------------------------------------------------------------ keys */

/** Where a key is made: the page names it and says where it came from. */
export type Purpose = "api" | "mcp";
/** Every capability (`*`), the read tools `mcp_setup` lists, or capability names typed in. */
export type Access = "all" | "read" | "named";
/** As long as the sign-in allows (empty), a fixed number of seconds, never, or seconds typed in. */
export type Lifetime = "" | "86400" | "604800" | "never" | "custom";

/** `mcp_setup.scopes`: the tool names a read-only key carries, and every capability's name. */
export type KeyScopes = { read_only: string[]; all: string[] };

export type KeyForm = {
  purpose: Purpose;
  name: string;
  access: Access;
  /** Comma-separated capability names, for `access: "named"`. */
  named: string;
  lifetime: Lifetime;
  /** Seconds, for `lifetime: "custom"`. */
  seconds: string;
  /** A static exception (lake#193): needs a reason. */
  isStatic: boolean;
  reason: string;
};

export const LIFETIMES: { value: Lifetime; label: string }[] = [
  { value: "", label: "As long as my sign-in allows (at most 30 days)" },
  { value: "86400", label: "1 day" },
  { value: "604800", label: "7 days" },
  { value: "custom", label: "A number of seconds" },
  { value: "never", label: "Never (a standing key)" },
];

/** The scopes `git`/`liquid` push and pull need (the hint Tokens has always given). */
export const LIQUID_SCOPES = "refs,find_missing,push,pull,upload_chunks,read_chunk";

/** "a, b,,c " → ["a", "b", "c"]: trimmed, empties dropped. */
export function commaSeparated(value: string): string[] {
  return value
    .split(",")
    .map((part) => part.trim())
    .filter(Boolean);
}

/** A key that never expires or a static one must say why it exists. */
export function needsReason(form: Pick<KeyForm, "lifetime" | "isStatic">): boolean {
  return form.lifetime === "never" || form.isStatic;
}

/** What is missing before the form can be sent, in words; null when it is complete. */
export function keyFormProblem(form: KeyForm): string | null {
  if (form.access === "named" && commaSeparated(form.named).length === 0) return "Name at least one capability the key may call.";
  if (form.lifetime === "custom") {
    const seconds = Number.parseInt(form.seconds, 10);
    if (!Number.isFinite(seconds) || seconds < 1) return "Give the lifetime in seconds, 1 or more.";
  }
  if (needsReason(form) && !form.reason.trim()) return "Say why this key exists: a standing or static key keeps its reason.";
  return null;
}

/** The `create_token` input for a key form, as Developer → MCP's handler (serve_mcp_key) built
 * it and Tokens builds it: *every tool* is `*`, except that a key that never expires must name
 * its scopes, so it names every capability (`scopes.all`); *read-only* is every read tool. */
export function keyInput(form: KeyForm, scopes: KeyScopes): Record<string, unknown> {
  const page = form.purpose === "mcp" ? "MCP" : "API";
  const readOnly = form.access === "read";
  const input: Record<string, unknown> = {
    name: form.name.trim() || (readOnly ? `${page} key (read-only)` : `${page} key`),
    scopes: form.access === "read" ? scopes.read_only : form.access === "named" ? commaSeparated(form.named) : ["*"],
    static: form.isStatic,
    reason: form.reason.trim() || `created on Developer → ${page}`,
  };
  if (form.lifetime === "never") {
    input.never_expires = true;
    if (form.access === "all") input.scopes = scopes.all;
  } else if (form.lifetime === "custom") {
    const seconds = Number.parseInt(form.seconds, 10);
    if (Number.isFinite(seconds) && seconds > 0) input.expires_in_seconds = seconds;
  } else if (form.lifetime) {
    input.expires_in_seconds = Number.parseInt(form.lifetime, 10);
  }
  return input;
}

/** What a token may call, in words: every tool, read-only, or the names it carries. */
export function accessOf(scopes: string[] | undefined, known?: KeyScopes): { label: string; title?: string; kind: "all" | "read" | "named" } {
  const list = scopes ?? [];
  if (list.includes("*")) return { label: "Every tool", kind: "all" };
  const same = (other: string[] | undefined) => !!other && other.length > 0 && other.length === list.length && other.every((name) => list.includes(name));
  if (same(known?.all)) return { label: "Every tool", title: "every capability, by name", kind: "all" };
  if (same(known?.read_only)) return { label: "Read-only", title: `${list.length} read tools`, kind: "read" };
  if (list.length === 0) return { label: "—", kind: "named" };
  if (list.length <= 3) return { label: list.join(", "), kind: "named" };
  return { label: `${list.length} capabilities`, title: list.join(", "), kind: "named" };
}

/** Active until it is revoked or its expiry passes. */
export function keyState(row: { revoked_at?: string | null; expires_at?: string | null }, now = Date.now()): "active" | "revoked" | "expired" {
  if (row.revoked_at) return "revoked";
  if (row.expires_at) {
    const at = Date.parse(row.expires_at);
    if (Number.isFinite(at) && at < now) return "expired";
  }
  return "active";
}

/* ------------------------------------------------------------------ operations */

export type Operation = {
  method: string;
  path: string;
  operationId: string;
  summary: string;
  description: string;
  /** The OpenAPI tag: `read`, `action`, `liquid-sync` or `mcp`. */
  tag: string;
  /** The product group `mcp_setup` puts its tool in, else a group named for its tag. */
  group: string;
  /** Changes something: refused to a read-only key. An action that only reads (workspace_query,
   * predict) declares `x-lake-action: read`. */
  writes: boolean;
  /** The permission the call needs on its object (`x-lake-action`). */
  needs?: string;
  public: boolean;
  op: Row;
};

const METHODS = ["get", "post", "put", "patch", "delete"];

/** Liquid sync calls sent as POST that only read (they carry no `x-lake-action`). */
const SYNC_READS = new Set(["find_missing", "pull"]);

const TAG_GROUPS: Record<string, string> = {
  "liquid-sync": "Liquid sync (binary)",
  mcp: "MCP",
};

/** Every operation of `/v1/openapi.json`, each in the product group of its MCP tool. */
export function operationsOf(doc: Row | undefined, groups?: { title: string; tools: ({ name: string } | string)[] }[]): Operation[] {
  const groupOf = new Map<string, string>();
  for (const group of groups ?? []) for (const tool of group.tools) groupOf.set(typeof tool === "string" ? tool : tool.name, group.title);
  const out: Operation[] = [];
  const paths = (doc?.paths ?? {}) as Record<string, Record<string, Row>>;
  for (const [path, item] of Object.entries(paths)) {
    for (const [method, op] of Object.entries(item ?? {})) {
      if (!METHODS.includes(method) || !op || typeof op !== "object") continue;
      const tag = String(op.tags?.[0] ?? "other");
      const id = String(op.operationId ?? "");
      const name = id.endsWith("_get") && !groupOf.has(id) ? id.slice(0, -4) : id;
      const needs = typeof op["x-lake-action"] === "string" ? (op["x-lake-action"] as string) : undefined;
      const upper = method.toUpperCase();
      out.push({
        method: upper,
        path,
        operationId: id,
        summary: String(op.summary ?? "") || String(op.description ?? "").split("\n")[0],
        description: String(op.description ?? ""),
        tag,
        group: groupOf.get(name) ?? TAG_GROUPS[tag] ?? "Other",
        writes: upper !== "GET" && needs !== "read" && !SYNC_READS.has(id) && id !== "mcp",
        needs,
        public: Array.isArray(op.security) && op.security.length === 0,
        op,
      });
    }
  }
  return out;
}

/** The operations matching `needle` (path, name, summary or group) in groups: `order` first, in
 * its order, then the rest alphabetically; within a group by path. */
export function groupOperations(ops: Operation[], needle: string, order: string[] = []): [string, Operation[]][] {
  const q = needle.trim().toLowerCase();
  const out = new Map<string, Operation[]>();
  for (const op of ops) {
    if (q && !`${op.path} ${op.operationId} ${op.summary} ${op.group}`.toLowerCase().includes(q)) continue;
    out.set(op.group, [...(out.get(op.group) ?? []), op]);
  }
  const rank = (group: string) => {
    const at = order.indexOf(group);
    return at === -1 ? order.length : at;
  };
  return [...out.entries()]
    .map(([group, list]) => [group, [...list].sort((a, b) => a.path.localeCompare(b.path) || a.method.localeCompare(b.method))] as [string, Operation[]])
    .sort(([a], [b]) => rank(a) - rank(b) || a.localeCompare(b));
}

/** One level of `$ref` into `components.schemas`, so a body shows its fields, not a pointer. */
export function resolveRef(schema: Row | undefined, components?: Record<string, Row>): Row | undefined {
  const ref = schema?.$ref;
  if (typeof ref !== "string") return schema;
  const name = ref.split("/").pop() ?? "";
  return components?.[name] ? { title: name, ...components[name] } : schema;
}

/** The request body's media type and schema, when the operation takes a body. */
export function requestBodyOf(op: Row, components?: Record<string, Row>): { type: string; schema?: Row } | null {
  const content = op.requestBody?.content as Record<string, Row> | undefined;
  if (!content) return null;
  const type = Object.keys(content)[0];
  if (!type) return null;
  return { type, schema: resolveRef(content[type]?.schema, components) };
}

/** The successful answer's schema. A JSON capability answers the envelope `{ok, value,
 * duration_ms}`; its `value` is the part that differs, so that is what is shown. */
export function responseSchemaOf(op: Row, components?: Record<string, Row>): { type: string; schema?: Row; enveloped: boolean } | null {
  const content = op.responses?.["200"]?.content as Record<string, Row> | undefined;
  if (!content) return null;
  const type = Object.keys(content)[0];
  if (!type) return null;
  const schema = resolveRef(content[type]?.schema, components);
  const value = schema?.properties?.value;
  return value && schema?.properties?.ok ? { type, schema: resolveRef(value, components), enveloped: true } : { type, schema, enveloped: false };
}

/** A minimal JSON value a schema accepts: its required fields, each a placeholder. */
export function skeleton(schema: Row | undefined, name = "value", depth = 0): unknown {
  if (!schema || depth > 3) return `<${name}>`;
  if (schema.example !== undefined) return schema.example;
  if (Array.isArray(schema.enum) && schema.enum.length) return schema.enum[0];
  const type = Array.isArray(schema.type) ? schema.type.find((t: string) => t !== "null") : schema.type;
  switch (type) {
    case "object": {
      const out: Record<string, unknown> = {};
      const props = (schema.properties ?? {}) as Record<string, Row>;
      for (const key of (schema.required ?? []) as string[]) out[key] = skeleton(props[key], key, depth + 1);
      return out;
    }
    case "array":
      return schema.minItems ? [skeleton(schema.items, name, depth + 1)] : [];
    case "integer":
    case "number":
      return schema.minimum ?? 1;
    case "boolean":
      return false;
    default:
      return `<${name}>`;
  }
}

function shellQuote(value: string): string {
  return `'${value.replace(/'/g, `'\\''`)}'`;
}

/** Copy-as-curl for one operation: path parameters and required query parameters as
 * placeholders, a JSON body with the required fields, the bearer as given. */
export function curlFor(op: Operation, origin: string, key: string, components?: Record<string, Row>): string {
  const path = op.path.replace(/\{([^}]+)\}/g, (_, n: string) => `<${n}>`);
  const query = ((op.op.parameters ?? []) as Row[])
    .filter((p) => p.in === "query" && p.required)
    .map((p) => `${p.name}=<${p.name}>`)
    .join("&");
  const url = `${origin}${path}${query ? `?${query}` : ""}`;
  const lines = [`curl -s${op.method === "GET" ? "" : ` -X ${op.method}`} ${query ? `"${url}"` : url}`];
  if (!op.public) lines.push(`  -H "Authorization: Bearer ${key}"`);
  const body = op.method === "GET" ? null : requestBodyOf(op.op, components);
  if (body) {
    lines.push(`  -H "Content-Type: ${body.type}"`);
    if (body.type === "application/json") lines.push(`  -d ${shellQuote(JSON.stringify(skeleton(body.schema)))}`);
    else lines.push(`  --data-binary @request.bin`);
  }
  return lines.join(" \\\n");
}

/** A schema as indented JSON for reading. */
export function schemaText(schema: unknown): string {
  return JSON.stringify(schema ?? {}, null, 2);
}
