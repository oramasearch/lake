// SPDX-License-Identifier: MIT

/** Data → MCP Connections (lake#652, docs/ui/mcp-connections.md): the pure parts the pages share, so
 * `npm test` covers them without a browser. A connection's tools come with the server's own JSON
 * Schemas; "Try it" turns one into a form and the form's values back into `arguments`. */

export type McpStatus = "new" | "starting" | "connected" | "needs_sign_in" | "failing";

export type McpTool = {
  connection_id: string;
  name: string;
  exported_name: string;
  title?: string;
  description?: string;
  input_schema?: JsonSchema;
  read_only_hint?: boolean | null;
  allowed: boolean;
  change: boolean;
  exported?: boolean;
  auto_approved?: { agent: string; granted_by?: string; granted_at?: string }[];
};

export type McpConnection = {
  id: string;
  name: string;
  prefix: string;
  url: string;
  transport: string;
  auth: { kind: "none" | "bearer" | "oauth"; secret?: { mount_id: string; name: string; field: string } | null; signed_in_at?: string | null; issuer?: string | null };
  status: McpStatus;
  last_error?: string | null;
  refreshed_at?: string | null;
  server?: { name?: string; version?: string; protocol_version?: string; instructions?: string } | null;
  negotiated_transport?: string | null;
  tool_count: number;
  allowed_count: number;
  exported: boolean;
  agent_id?: string | null;
  streams?: { input_topic?: string | null; output_topic?: string | null; notifications_topic?: string | null; subscribe_resources?: string[] };
  calls_this_month?: number;
  tools?: McpTool[];
  resources?: { uri: string; name?: string; description?: string; mime_type?: string }[];
  prompts?: { name: string; description?: string }[];
  skipped?: string[];
  created_at?: string;
  /** `stdio`: a server Lake runs as a workspace App (lake#659); `remote`: one at a URL. */
  kind?: "remote" | "stdio";
  app_id?: string | null;
  stdio?: McpStdio | null;
  /** The App's state, for a stdio connection. */
  app?: { phase?: string; ready?: boolean; desired?: string; message?: string } | null;
};

/** What a stdio connection runs (lake#659): the package, how, and the names of its variables. */
export type McpStdio = {
  runtime: "node" | "python";
  package: string;
  command?: string | null;
  args?: string[];
  env_keys?: string[];
  secret_env_keys?: string[];
  mount_id?: string;
  app_name?: string;
};

// eslint-disable-next-line @typescript-eslint/no-explicit-any
export type JsonSchema = Record<string, any>;

const STATUS_LABELS: Record<string, string> = {
  connected: "Connected",
  starting: "Starting",
  needs_sign_in: "Needs sign-in",
  failing: "Failing",
  new: "New",
};

export function statusLabel(status: string | null | undefined): string {
  return STATUS_LABELS[status ?? ""] ?? "Unknown";
}

/** The chip tone a status reads in. */
export function statusTone(status: string | null | undefined): "ok" | "warn" | "bad" | "neutral" {
  if (status === "connected") return "ok";
  if (status === "needs_sign_in") return "warn";
  if (status === "failing") return "bad";
  return "neutral";
}

export function runtimeLabel(runtime: string | null | undefined): string {
  return runtime === "python" ? "Python package" : "npm package";
}

/** The command line a stdio connection's App runs, as people read it. */
export function stdioCommandLine(stdio: McpStdio): string {
  const runner = stdio.runtime === "python" ? "uvx" : "npx";
  const head = stdio.command ? (stdio.runtime === "python" ? [runner, "--from", stdio.package, stdio.command] : [runner, "--package", stdio.package, stdio.command]) : [runner, stdio.package];
  return [...head, ...(stdio.args ?? [])].join(" ");
}

/** The arguments typed one per line; blank lines dropped. */
export function argsOf(text: string): string[] {
  return text
    .split("\n")
    .map((line) => line.trim())
    .filter(Boolean);
}

/** Why an environment variable's name cannot be used, or null. Lake keeps MCP_, APP_ and LAKE_ for
 * itself and the App. */
export function envNameProblem(name: string): string | null {
  if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(name)) return "Letters, digits and _, not starting with a digit";
  if (/^(MCP_|APP_|LAKE_)/i.test(name)) return "MCP_, APP_ and LAKE_ are Lake's";
  return null;
}

export function authLabel(kind: string | null | undefined): string {
  if (kind === "bearer") return "Bearer token";
  if (kind === "oauth") return "OAuth sign-in";
  return "No authentication";
}

export function transportLabel(transport: string | null | undefined): string {
  if (transport === "streamable_http") return "Streamable HTTP";
  if (transport === "sse") return "HTTP+SSE";
  return "Automatic";
}

/** The host a URL names, for a card's second line; the URL itself when it does not parse. */
export function hostOf(url: string): string {
  try {
    return new URL(url).host || url;
  } catch {
    return url;
  }
}

/** The prefix Lake gives a connection's tools, as `mcp_connection_prefix` makes it: lower-case
 * letters, digits and underscores, starting with a letter, at most 24 bytes. Shown while a name is
 * typed so the person sees the tool names they will get. */
export function prefixOf(name: string): string {
  let out = "";
  let gap = false;
  for (const c of name) {
    if (/[A-Za-z0-9]/.test(c)) {
      if (gap && out) out += "_";
      gap = false;
      if (!out && /[0-9]/.test(c)) continue;
      out += c.toLowerCase();
    } else {
      gap = true;
    }
    if (out.length >= 24) break;
  }
  return out.replace(/_+$/, "");
}

/** One field of a "Try it" form, from one property of the tool's input schema. */
export type FieldModel = {
  name: string;
  label: string;
  kind: "text" | "number" | "integer" | "boolean" | "choice" | "json";
  required: boolean;
  description?: string;
  options?: string[];
  placeholder?: string;
};

/** The form for a tool's input schema: a field per property, in the schema's order. Strings, numbers,
 * booleans and enums get their own control; anything else (objects, arrays, unions) is typed as
 * JSON. A schema without properties is one JSON field for the whole arguments object. */
export function formFields(schema: JsonSchema | null | undefined): FieldModel[] {
  const properties = schema && typeof schema === "object" ? schema.properties : null;
  if (!properties || typeof properties !== "object" || !Object.keys(properties).length) {
    return [{ name: "", label: "Arguments (JSON)", kind: "json", required: false, placeholder: "{}" }];
  }
  const required = new Set<string>(Array.isArray(schema?.required) ? schema!.required : []);
  return Object.entries(properties as Record<string, JsonSchema>).map(([name, property]) => {
    const type = Array.isArray(property?.type) ? property.type.find((t: string) => t !== "null") : property?.type;
    const field: FieldModel = {
      name,
      label: typeof property?.title === "string" && property.title ? property.title : name,
      kind: "json",
      required: required.has(name),
      description: typeof property?.description === "string" ? property.description : undefined,
    };
    if (Array.isArray(property?.enum) && property.enum.every((v: unknown) => typeof v === "string")) {
      field.kind = "choice";
      field.options = property.enum;
    } else if (type === "string") {
      field.kind = "text";
    } else if (type === "integer") {
      field.kind = "integer";
    } else if (type === "number") {
      field.kind = "number";
    } else if (type === "boolean") {
      field.kind = "boolean";
    } else {
      field.placeholder = type === "array" ? "[]" : "{}";
    }
    return field;
  });
}

export type FormValues = Record<string, string | boolean>;

/** The `arguments` a form's values make, or the first problem with them (`{field, message}`). An
 * empty optional field is left out; a required one is a problem. */
export function argumentsFrom(fields: FieldModel[], values: FormValues): { arguments: Record<string, unknown> } | { field: string; message: string } {
  if (fields.length === 1 && fields[0].name === "") {
    const raw = String(values[""] ?? "").trim();
    if (!raw) return { arguments: {} };
    try {
      const parsed = JSON.parse(raw);
      if (!parsed || typeof parsed !== "object" || Array.isArray(parsed)) return { field: "", message: "The arguments are a JSON object." };
      return { arguments: parsed };
    } catch {
      return { field: "", message: "The arguments are not valid JSON." };
    }
  }
  const out: Record<string, unknown> = {};
  for (const field of fields) {
    const value = values[field.name];
    if (field.kind === "boolean") {
      if (value === true || value === false) out[field.name] = value;
      else if (field.required) out[field.name] = false;
      continue;
    }
    const text = typeof value === "string" ? value.trim() : "";
    if (!text) {
      if (field.required) return { field: field.name, message: `${field.label} is required.` };
      continue;
    }
    if (field.kind === "integer" || field.kind === "number") {
      const number = Number(text);
      if (!Number.isFinite(number) || (field.kind === "integer" && !Number.isInteger(number))) {
        return { field: field.name, message: `${field.label} is ${field.kind === "integer" ? "a whole number" : "a number"}.` };
      }
      out[field.name] = number;
    } else if (field.kind === "json") {
      try {
        out[field.name] = JSON.parse(text);
      } catch {
        return { field: field.name, message: `${field.label} is not valid JSON.` };
      }
    } else {
      out[field.name] = typeof value === "string" ? value : text;
    }
  }
  return { arguments: out };
}

/** What a tool answered, as text a person reads: each text item of `content`, and the structured
 * content as JSON when there is no text. */
export function resultText(result: { content?: { type?: string; text?: string }[]; structured_content?: unknown } | null | undefined): string {
  if (!result) return "";
  const texts = (result.content ?? []).filter((c) => c?.type === "text" && typeof c.text === "string").map((c) => c.text as string);
  if (texts.length) return texts.join("\n\n");
  if (result.structured_content != null) return JSON.stringify(result.structured_content, null, 2);
  return JSON.stringify(result.content ?? [], null, 2);
}

/** The sign-in callback's state, `<customer>.<workspace>.<random>`: which space and workspace to
 * finish it in. Null when it is not one Lake made. */
export function signInState(state: string | null | undefined): { customerId: string; workspaceId: string } | null {
  if (!state) return null;
  const parts = state.split(".");
  if (parts.length !== 3 || parts.some((p) => !/^[0-9a-f-]{8,64}$/i.test(p))) return null;
  return { customerId: parts[0], workspaceId: parts[1] };
}

/** How a call came, for the recent calls table. */
export function viaLabel(via: string | null | undefined, agent?: string | null): string {
  if (via === "reexport") return "Lake's MCP";
  if (via === "agent") return agent ? `Agent ${agent}` : "An agent";
  if (via === "person") return "A person";
  return "—";
}

/** What the re-export switch says before it is turned on. */
export function exportWarning(serverName: string): string {
  return `Anyone who may call Lake's tools in this workspace will be able to call these tools, and Lake calls ${serverName} for them with this connection's credential. Lake's permission on the workspace is the only gate.`;
}
