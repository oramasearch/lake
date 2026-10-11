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

/* ------------------------------------------------------------------ MCP clients */

/** The MCP clients Developer → MCP sets up, in the order its picker shows them. */
export type ClientId = "claude-code" | "codex" | "claude-desktop" | "cursor" | "vscode" | "windsurf" | "gemini" | "curl" | "web";

export const CLIENTS: { id: ClientId; name: string; how: string }[] = [
  { id: "claude-code", name: "Claude Code", how: "One command: HTTP, the key as a header" },
  { id: "codex", name: "OpenAI Codex", how: "CLI and IDE: config.toml, the key from your environment" },
  { id: "claude-desktop", name: "Claude Desktop", how: "Its config file, through mcp-remote" },
  { id: "cursor", name: "Cursor", how: "mcp.json: the URL and a header" },
  { id: "vscode", name: "VS Code", how: "Copilot agent mode: asks for the key once" },
  { id: "windsurf", name: "Windsurf", how: "mcp_config.json: serverUrl and a header" },
  { id: "gemini", name: "Gemini CLI", how: "settings.json: httpUrl and a header" },
  { id: "curl", name: "Any client", how: "JSON-RPC 2.0 over HTTP, shown with curl" },
  { id: "web", name: "ChatGPT and Claude.ai", how: "Web connectors: what works today" },
];

/** One copyable block of a client's setup. `id` names the code element and its Copy button. */
export type SetupBlock = { id: string; title: string; code: string; note?: string };

/** A client's whole setup: where it goes, the blocks, how to see it worked, and what to know. */
export type ClientSetup = {
  /** The file (or files, per platform) the configuration goes in; none for a command. */
  where: { label: string; path: string }[];
  blocks: SetupBlock[];
  /** One line on how to see lake connected. */
  check: string;
  notes: string[];
};

/** The name every client knows lake's server by. */
export const SERVER_NAME = "lake";
/** The environment variable the setups read the key from when none was made or pasted. */
export const KEY_ENV = "LAKE_KEY";

const json = (value: unknown) => JSON.stringify(value, null, 2);

/** The exact setup for one client against `endpoint` (lake's `/mcp`), with `key` filled in, or
 * read from `$LAKE_KEY` (in the syntax each client expands) when there is no key yet. */
export function clientSetup(client: ClientId, endpoint: string, key: string | null): ClientSetup {
  const shellKey = key ?? `$${KEY_ENV}`;
  switch (client) {
    case "claude-code":
      return {
        where: [{ label: "Saved by the command in", path: "~/.claude.json" }],
        blocks: [
          {
            id: "mcp-claude-code",
            title: "terminal",
            code: `claude mcp add --transport http ${SERVER_NAME} ${endpoint} --header "Authorization: Bearer ${shellKey}"`,
            note: "Add --scope user to use Lake in every project, or --scope project to share it through the project's .mcp.json (without your key in it).",
          },
        ],
        check: "Run /mcp inside Claude Code, or claude mcp list in a terminal: Lake shows ✔ Connected.",
        notes: [],
      };
    case "codex":
      return {
        where: [{ label: "Configuration", path: "~/.codex/config.toml" }],
        blocks: [
          {
            id: "mcp-codex-config",
            title: "~/.codex/config.toml",
            code: `[mcp_servers.${SERVER_NAME}]\nurl = "${endpoint}"\nbearer_token_env_var = "${KEY_ENV}"`,
            note: "The CLI and the IDE extension share this file. Codex reads the key from the environment, so it is never written into the file.",
          },
          {
            id: "mcp-codex-env",
            title: "your shell profile",
            code: `export ${KEY_ENV}="${key ?? "<your Lake key>"}"`,
          },
          {
            id: "mcp-codex-add",
            title: "or add it with one command",
            code: `codex mcp add ${SERVER_NAME} --url ${endpoint} --bearer-token-env-var ${KEY_ENV}`,
          },
        ],
        check: "Run codex mcp list in a terminal, or /mcp inside Codex: Lake is listed with its tools.",
        notes: [],
      };
    case "claude-desktop":
      return {
        where: [
          { label: "macOS", path: "~/Library/Application Support/Claude/claude_desktop_config.json" },
          { label: "Windows", path: "%APPDATA%\\Claude\\claude_desktop_config.json" },
        ],
        blocks: [
          {
            id: "mcp-claude-desktop",
            title: "claude_desktop_config.json",
            code: json({
              mcpServers: {
                [SERVER_NAME]: {
                  command: "npx",
                  args: ["-y", "mcp-remote", endpoint, "--header", "Authorization:${LAKE_AUTH_HEADER}"],
                  env: { LAKE_AUTH_HEADER: `Bearer ${key ?? `<${KEY_ENV}>`}` },
                },
              },
            }),
            note: "Claude Desktop → Settings → Developer → Edit Config opens the file. mcp-remote (Node.js 18 or later) bridges the app to Lake over HTTP; the header goes through env so the space in it survives on Windows.",
          },
        ],
        check: "Restart Claude Desktop; Lake appears under the tools menu of a new chat, and Settings → Developer shows it running.",
        notes: key ? [] : [`Put your key in place of <${KEY_ENV}>: Claude Desktop does not read your shell's environment.`],
      };
    case "cursor":
      return {
        where: [
          { label: "Every project", path: "~/.cursor/mcp.json" },
          { label: "One project", path: ".cursor/mcp.json" },
        ],
        blocks: [
          {
            id: "mcp-cursor",
            title: "~/.cursor/mcp.json",
            code: json({ mcpServers: { [SERVER_NAME]: { url: endpoint, headers: { Authorization: `Bearer ${key ?? `\${env:${KEY_ENV}}`}` } } } }),
          },
        ],
        check: "Cursor → Settings → Tools & Integrations (MCP) lists Lake with a green dot and its tools.",
        notes: key ? [] : [`\${env:${KEY_ENV}} is read from the environment Cursor was started in.`],
      };
    case "vscode":
      return {
        where: [{ label: "The workspace", path: ".vscode/mcp.json" }],
        blocks: [
          {
            id: "mcp-vscode",
            title: ".vscode/mcp.json",
            code: json({
              servers: { [SERVER_NAME]: { type: "http", url: endpoint, headers: { Authorization: "Bearer ${input:lake-key}" } } },
              inputs: [{ type: "promptString", id: "lake-key", description: "Lake MCP key", password: true }],
            }),
            note: key
              ? "VS Code asks for the key the first time Lake starts and keeps it in its secret storage: paste the key from step 1. It never goes into the file."
              : "VS Code asks for the key the first time Lake starts and keeps it in its secret storage. It never goes into the file, so the file is safe to commit.",
          },
        ],
        check: "Run MCP: List Servers from the Command Palette: Lake is Running. Its tools appear in Copilot Chat's agent mode tool picker.",
        notes: [],
      };
    case "windsurf":
      return {
        where: [
          { label: "Windsurf", path: "~/.codeium/windsurf/mcp_config.json" },
          { label: "Devin Desktop", path: "~/.config/devin/mcp_config.json" },
        ],
        blocks: [
          {
            id: "mcp-windsurf",
            title: "mcp_config.json",
            code: json({ mcpServers: { [SERVER_NAME]: { serverUrl: endpoint, headers: { Authorization: `Bearer ${key ?? `\${env:${KEY_ENV}}`}` } } } }),
            note: "Remote servers take serverUrl, not url. Cascade → MCP servers → Refresh picks the file up.",
          },
        ],
        check: "Cascade's MCP panel lists Lake as enabled, with its tools.",
        notes: [],
      };
    case "gemini":
      return {
        where: [
          { label: "Every project", path: "~/.gemini/settings.json" },
          { label: "One project", path: ".gemini/settings.json" },
        ],
        blocks: [
          {
            id: "mcp-gemini",
            title: "~/.gemini/settings.json",
            code: json({ mcpServers: { [SERVER_NAME]: { httpUrl: endpoint, headers: { Authorization: `Bearer ${key ?? `\${${KEY_ENV}}`}` } } } }),
            note: "httpUrl is streamable HTTP (url would be SSE). Merge mcpServers into the settings you have.",
          },
          {
            id: "mcp-gemini-add",
            title: "or add it with one command",
            code: `gemini mcp add --transport http --header "Authorization: Bearer ${shellKey}" ${SERVER_NAME} ${endpoint}`,
          },
        ],
        check: "Run /mcp inside Gemini CLI, or gemini mcp list in a terminal: Lake is Connected.",
        notes: [],
      };
    case "curl":
      return {
        where: [],
        blocks: [
          {
            id: "mcp-curl-initialize",
            title: "initialize",
            code: `curl -s ${endpoint} \\\n  -H "Authorization: Bearer ${shellKey}" \\\n  -H 'Content-Type: application/json' \\\n  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"curl","version":"1"}}}'`,
          },
          {
            id: "mcp-curl-tools",
            title: "tools/list",
            code: `curl -s ${endpoint} \\\n  -H "Authorization: Bearer ${shellKey}" \\\n  -H 'Content-Type: application/json' \\\n  -d '{"jsonrpc":"2.0","id":2,"method":"tools/list"}'`,
            note: "tools/list needs no key; tools/call on anything but server_info does.",
          },
          {
            id: "mcp-curl-call",
            title: "tools/call",
            code: `curl -s ${endpoint} \\\n  -H "Authorization: Bearer ${shellKey}" \\\n  -H 'Content-Type: application/json' \\\n  -d '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"list_customers","arguments":{"limit":5}}}'`,
            note: "A refused call is a result with isError: true and the same error envelope REST answers.",
          },
        ],
        check: "initialize answers Lake's serverInfo; tools/list answers every tool.",
        notes: ["Any client that speaks MCP's streamable HTTP transport and can send an Authorization header connects the same way."],
      };
    case "web":
      return {
        where: [],
        blocks: [
          {
            id: "mcp-claude-ai",
            title: "Claude.ai → Customize → Connectors → Add custom connector",
            code: `Name:            ${SERVER_NAME}\nRemote MCP URL:  ${endpoint}\nRequest header:  Authorization: Bearer ${key ?? `<${KEY_ENV}>`}`,
            note: "Claude.ai's custom connectors can send a fixed request header on every call. Claude's servers call Lake, so Lake must be reachable from the internet, and the key is kept by Anthropic: give it a read-only key.",
          },
        ],
        check: "The connector shows Lake's tools; turn it on in a chat from the tools menu.",
        notes: [
          "ChatGPT's custom connectors (developer mode) sign in with OAuth or no authentication, and a Lake key is a bearer token, so ChatGPT cannot connect to Lake yet. Use OpenAI Codex, which takes the key from your environment.",
          "Lake has no OAuth sign-in for connectors.",
        ],
      };
  }
}

/** How to give an agent the lake skill: the public `SKILL.md` and where each agent reads it. */
export type SkillTarget = "claude-code" | "codex" | "gemini" | "other";

export const SKILL_TARGETS: { id: SkillTarget; label: string }[] = [
  { id: "claude-code", label: "Claude Code" },
  { id: "codex", label: "Codex (AGENTS.md)" },
  { id: "gemini", label: "Gemini CLI (GEMINI.md)" },
  { id: "other", label: "Any agent" },
];

/** The skill install for one agent, from lake's public `/skills/lake/SKILL.md`. */
export function skillInstall(target: SkillTarget, origin: string): SetupBlock[] {
  const skill = `${origin}/skills/lake/SKILL.md`;
  const pointer = (file: string, home: string) =>
    `mkdir -p ${home} && curl -fsSL ${skill} -o ${home}/lake-SKILL.md\n` +
    `cat >> ${home}/${file} <<'EOF'\n\n` +
    `## Lake\n` +
    `Lake is connected as the MCP server "lake". Before using its tools, read\n` +
    `${home}/lake-SKILL.md: what Lake is, which tool does what, the worked flows\n` +
    `and the rules (an edited pipeline is proposed again; deleting destroys data).\n` +
    `EOF`;
  switch (target) {
    case "claude-code":
      return [
        {
          id: "skill-claude-code",
          title: "install the skill for Claude Code",
          code: `mkdir -p ~/.claude/skills/lake && curl -fsSL ${skill} -o ~/.claude/skills/lake/SKILL.md`,
          note: "Claude Code loads ~/.claude/skills/lake/SKILL.md in every project; use .claude/skills/lake/ for one project.",
        },
      ];
    case "codex":
      return [{ id: "skill-codex", title: "point Codex at the skill (~/.codex/AGENTS.md)", code: pointer("AGENTS.md", "~/.codex"), note: "Codex reads ~/.codex/AGENTS.md in every session; a repository's own AGENTS.md works the same way." }];
    case "gemini":
      return [{ id: "skill-gemini", title: "point Gemini CLI at the skill (~/.gemini/GEMINI.md)", code: pointer("GEMINI.md", "~/.gemini"), note: "Gemini CLI loads ~/.gemini/GEMINI.md as context in every session." }];
    case "other":
      return [
        {
          id: "skill-other",
          title: "Lake for any agent",
          code: `curl -fsSL ${origin}/llms.txt\ncurl -fsSL ${origin}/llms-full.txt   # every tool with its input schema\ncurl -fsSL ${skill}`,
          note: "All three are public. Put the one your agent can hold in its context or rules file.",
        },
      ];
  }
}
