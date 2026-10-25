/** The quickstart process as pure rules (lake#1128, docs/ui/quickstart.md), so `npm test` runs them
 * (tests/quickstart.test.ts): what a dropped file is sent as, the columns a first model trains on,
 * how often the page reads the status, when it publishes a trained model, the key put into the
 * website code, and whether the balance lets a paid step start. No React here. */

/** A step as quickstart_status answers it. */
export type StepState = "pending" | "running" | "done" | "failed" | "skipped";
export type Step = { step: string; title: string; state: StepState; error: { code: string; message: string } | null; note?: string; progress?: { status?: string; started_at?: string } | null };
export type QuickstartState = "waiting_for_workspace" | "not_started" | "in_progress" | "search_ready" | "training" | "trained" | "done" | "failed";
export type Status = {
  quickstart_id: string | null;
  customer_id: string;
  workspace_id: string;
  state: QuickstartState;
  next: string;
  steps: Step[];
  dataset: { id: string; database: string; name: string; rows?: number | null; columns?: Column[] } | null;
  index: { id: string; fields: string[]; published: boolean } | null;
  training: {
    run_id?: string;
    target?: string | null;
    target_value?: string | null;
    entity_column?: string | null;
    methodology?: string;
    as_of?: string;
    preparation?: Preparation | null;
    /** A target derived from a column (lake#1182). */
    derived_target?: { column: string; name: string; rule: string } | null;
  } | null;
  /** The date the dataset's dates are read as of (lake#1181). */
  snapshot_date?: string | null;
  /** The column to predict (and value) and the row's name offered first. */
  suggested?: { target: string | null; target_value: string | null; entity_column: string | null } | null;
  /** `serves: false`: a clustering model, trained but not published for the website (lake#1182). */
  model: { id: string; published: boolean; serves?: boolean } | null;
  /** The Ask Orama conversation that decides what to build (start_quickstart_conversation, lake#1182). */
  conversation?: Conversation | null;
  workspace?: { ready?: boolean; phase?: string; message?: string };
  quickstarts?: { quickstart_id: string; dataset_id: string | null; created_at: string | null }[];
  costs?: Costs;
};
/** The quickstart's Ask Orama conversation as quickstart_status names it: `page` goes with every
 * message, so the agent always knows the dataset (lake#1182). */
export type Conversation = {
  conversation_id: string;
  task_id: string | null;
  started_at?: string | null;
  page: Record<string, string>;
};
/** A dataset column with its detected kind (lake#1181, docs/api/quickstart.md#columns). `kind` is
 * null when the engine could not profile it. */
export type Column = {
  name: string;
  type: string;
  kind?: string | null;
  title?: string;
  distinct_count?: number;
  values?: string[];
  feature?: { use: boolean; reason?: string };
  predict?: { can: boolean; reason?: string; values?: string[]; choose_value?: boolean; positive?: string };
};
/** How a training was prepared: the copy it trained on, what was left out and why. */
export type Preparation = {
  copy: boolean;
  dataset_id?: string | null;
  entity?: string;
  used?: string[];
  dropped?: { name: string; reason: string }[];
  label?: { column: string; positive: string } | null;
  note?: string | null;
  applied?: string[];
};
export type Rate = { unit_micros: number; per: number } | null;
export type Costs = {
  billing: boolean;
  currency: string;
  balance: { state: string; balance_micros: number; add_micros: number; add_funds_url: string; exempt?: string } | null;
  steps: { step: string; capabilities: string[]; billed_by: { dimension: string; title: string; unit: string; rate: Rate }[]; note: string }[];
};
export type Snippet = { id: string; title: string; language: string; code: string };
export type Snippets = {
  quickstart_id: string;
  origin: string;
  ready: boolean;
  includes_model: boolean;
  snippets: Snippet[];
  /** One block for a coding agent to install it (lake#1185): the source, ids, key and context. */
  prompt?: string;
  /** The browser key the code needs (lake#1152): `secret` once, when the call made it with `origins`. */
  key: {
    kind: "browser_key";
    placeholder: string;
    capabilities: string[];
    create: { capability: string; input: Record<string, unknown> };
    secret: string | null;
    browser_key: BrowserKey | null;
    note: string;
  } | null;
  next?: string;
};

/** A browser key as create_browser_key and list_browser_keys answer it (docs/api/browser-keys.md). */
export type BrowserKey = {
  id: string;
  name: string;
  workspace_id: string;
  index_id: string;
  model_id: string | null;
  origins: string[];
  capabilities: string[];
  rate_limit_per_minute: number;
  state: "active" | "revoked" | "expired";
  created_by?: string;
  created_at?: string;
  expires_at: string | null;
  revoked_at: string | null;
  last_used_at: string | null;
};

/** A website's origin as a browser key lists it, from what a person typed (`www.shop.com`,
 * `https://shop.com/products`): https unless it is this computer, lowercase, no path. Lake checks it
 * again; this only says early what it will refuse. */
export function siteOrigin(typed: string): { origin: string } | { error: string } {
  let text = typed.trim();
  if (!text) return { error: "Type your website's address, such as https://www.example.com." };
  if (!/^[a-z][a-z0-9+.-]*:\/\//i.test(text)) text = (/^(localhost|127\.0\.0\.1)(:|$)/i.test(text) ? "http://" : "https://") + text;
  let url: URL;
  try {
    url = new URL(text);
  } catch {
    return { error: "That is not a website address." };
  }
  const local = url.hostname === "localhost" || url.hostname === "127.0.0.1";
  if (url.protocol !== "https:" && !(url.protocol === "http:" && local)) return { error: "A website's address starts with https:// (http:// only for localhost)." };
  return { origin: `${url.protocol}//${url.host}`.toLowerCase() };
}

/** The most a dataset file may hold: what upload_dataset takes. */
export const MAX_UPLOAD_BYTES = 16 * 1024 * 1024;

/** What a file is sent as, by its name, before it is read: csv, jsonl, a JSON document to turn into
 * lines, or why it cannot be. */
export function fileKind(name: string): { kind: "csv" | "jsonl" | "json" } | { error: string } {
  const lower = name.toLowerCase();
  if (lower.endsWith(".csv")) return { kind: "csv" };
  if (lower.endsWith(".jsonl") || lower.endsWith(".ndjson")) return { kind: "jsonl" };
  if (lower.endsWith(".json")) return { kind: "json" };
  if (lower.endsWith(".parquet")) return { error: "Parquet files are not loaded here yet. Save it as CSV or JSON Lines and drop that instead." };
  return { error: "Drop a CSV, JSON or JSON Lines file." };
}

/** The text upload_dataset takes for a file's text: CSV and JSON Lines as they are; a JSON array of
 * objects (or one object) as one line per object. */
export function uploadContent(name: string, text: string): { content: string; format: "csv" | "jsonl" } | { error: string } {
  const kind = fileKind(name);
  if ("error" in kind) return kind;
  if (!text.trim()) return { error: "The file is empty." };
  if (kind.kind === "csv") return { content: text, format: "csv" };
  if (kind.kind === "jsonl") return { content: text, format: "jsonl" };
  let parsed: unknown;
  try {
    parsed = JSON.parse(text);
  } catch {
    return { error: "The file is not valid JSON." };
  }
  const rows = Array.isArray(parsed) ? parsed : [parsed];
  if (!rows.length) return { error: "The JSON array is empty." };
  if (!rows.every((row) => row && typeof row === "object" && !Array.isArray(row))) return { error: "Each item of the JSON array must be an object, one row each." };
  return { content: rows.map((row) => JSON.stringify(row)).join("\n") + "\n", format: "jsonl" };
}

/** The columns a first model is trained with, until the person picks others: the last column of
 * the file is the outcome, and the first text column names a row. `columns` are the file's, in its
 * order; without them, the index's fields. */
export function firstColumns(fields: string[], columns: { name: string; type: string }[] = []): { target: string; entity: string } {
  const names = columns.length ? columns.map((c) => c.name) : fields;
  if (!names.length) return { target: "", entity: "" };
  const target = names[names.length - 1];
  if (names.length === 1) return { target, entity: "" };
  const text = columns.find((c) => c.name !== target && c.type === "string");
  return { target, entity: text ? text.name : names[0] };
}

/** Whether every column carries a detected kind, so the page offers the detected choices. */
export function detected(columns: Column[] | undefined): boolean {
  return !!columns && columns.length > 0 && columns.every((c) => typeof c.kind === "string");
}

/** The columns the model can predict, each with its detected type, as the choice lists them. */
export function predictable(columns: Column[]): { name: string; label: string }[] {
  return columns.filter((c) => c.predict?.can).map((c) => ({ name: c.name, label: c.title ? `${c.name} (${c.title})` : c.name }));
}

/** A column's detected type, as the column list shows it. */
export function kindLabel(column: Column): string {
  return column.title ?? column.type ?? "";
}

/** What the person has chosen for training. */
export type Choice = { target: string; value: string; entity: string; asOf: string };

/** The choices the page starts with: what the last training used, else what Lake suggests, else
 * (an engine that could not profile the columns) the file's order. */
export function startingChoice(status: Pick<Status, "dataset" | "index" | "training" | "suggested" | "snapshot_date">): Choice {
  const columns = status.dataset?.columns ?? [];
  const asOf = dateInput(status.training?.as_of ?? status.snapshot_date ?? "");
  if (!detected(columns)) {
    const guess = firstColumns(columns.length ? columns.map((c) => c.name) : status.index?.fields ?? [], columns);
    return { target: status.training?.target ?? guess.target, value: "", entity: status.training?.entity_column ?? guess.entity, asOf };
  }
  const can = new Set(predictable(columns).map((c) => c.name));
  const trained = status.training?.target && can.has(status.training.target) ? status.training : null;
  const target = trained?.target ?? status.suggested?.target ?? "";
  const value = trained ? trained.target_value ?? "" : status.suggested?.target === target ? status.suggested?.target_value ?? "" : "";
  const entity = trained ? trained.entity_column ?? "" : status.suggested?.entity_column ?? "";
  return { target, value: value || valuesOf(columns, target)[0] || "", entity, asOf };
}

/** The values offered for a category to predict; empty for a column with two values. */
export function valuesOf(columns: Column[], target: string): string[] {
  const column = columns.find((c) => c.name === target);
  return column?.predict?.choose_value ? column.predict.values ?? [] : [];
}

/** What happens to each column with these choices, in the file's order. */
export function columnUses(columns: Column[], choice: Pick<Choice, "target" | "entity">): { name: string; kind: string; use: string; reason: string; used: boolean }[] {
  return columns.map((c) => {
    const kind = kindLabel(c);
    if (c.name === choice.target) return { name: c.name, kind, use: "Predicted", reason: "", used: true };
    if (c.name === choice.entity) return { name: c.name, kind, use: "Names a row", reason: "", used: true };
    if (c.feature?.use) return { name: c.name, kind, use: "Learned from", reason: c.feature.reason ?? "", used: true };
    return { name: c.name, kind, use: "Left out", reason: c.feature?.reason ?? "", used: false };
  });
}

/** A date or ISO time as a date field's value (YYYY-MM-DD); empty when it is not one. */
export function dateInput(value: string): string {
  return /^\d{4}-\d{2}-\d{2}/.test(value) ? value.slice(0, 10) : "";
}

/** train_quickstart's input for these choices: as_of only when the date was changed. */
export function trainInput(choice: Choice, columns: Column[], snapshot: string | null | undefined): Record<string, string> {
  const input: Record<string, string> = { target: choice.target };
  if (choice.value && valuesOf(columns, choice.target).length) input.target_value = choice.value;
  if (choice.entity) input.entity_column = choice.entity;
  if (choice.asOf && choice.asOf !== dateInput(snapshot ?? "")) input.as_of = choice.asOf;
  return input;
}

/** The train button's words: a first training, or another with changed choices after a failure. */
export function trainLabel(failed: boolean): string {
  return failed ? "Train with these choices" : "Train a model";
}

/** How often the page reads the status: often while the workspace starts or the model trains,
 * not at all otherwise. */
export function pollInterval(state: QuickstartState | undefined): number | false {
  if (state === "waiting_for_workspace") return 5000;
  if (state === "training") return 3000;
  return false;
}

/** A trained model is published by the same click that trained it: once the status says trained,
 * the page calls train_quickstart again. */
export function publishesNow(status: Pick<Status, "state"> | null | undefined): boolean {
  return status?.state === "trained";
}

/** The step a person is on: the first that is not done. */
export function currentStep(steps: Step[]): string {
  return steps.find((s) => s.state !== "done")?.step ?? "done";
}

/** The website code with the key put where it says the placeholder, when there is a key. */
export function withKey(code: string, placeholder: string, secret: string | null): string {
  return secret ? code.split(placeholder).join(secret) : code;
}

/** Whether a key made with `made` scopes can do everything the code now asks (`needed`). */
export function keyCovers(made: string[], needed: string[]): boolean {
  return needed.every((scope) => made.includes(scope));
}

/** Whether a paid step may start, or the balance blocks it and the page offers the top-up link. */
export function blockedBy(balance: { state?: string; exempt?: string } | null | undefined): boolean {
  return !!balance && !balance.exempt && balance.state === "blocked";
}

/** A rate in words: "$0.002 per 60,000 mcpu seconds". Null: no published rate. */
export function rateLabel(rate: Rate, unit: string, money: (micros: number) => string): string | null {
  if (!rate) return null;
  const words = unit.replace(/_/g, " ");
  if (rate.per === 1) return `${money(rate.unit_micros)} per ${words.replace(/s$/, "")}`;
  return `${money(rate.unit_micros)} per ${rate.per.toLocaleString("en-US")} ${words}`;
}

/** The status read's tone for a step's state. */
export function stepTone(state: StepState): "success" | "info" | "danger" | "neutral" {
  if (state === "skipped") return "neutral";
  return state === "done" ? "success" : state === "running" ? "info" : state === "failed" ? "danger" : "neutral";
}

/** A search hit's row in one line, as the website snippet shows it. */
export function hitLine(fields: Record<string, unknown> | undefined): string {
  return Object.values(fields ?? {})
    .map((value) => (value === null || value === undefined ? "" : String(value)))
    .filter(Boolean)
    .join(" · ");
}

/** The website step's tabs (lake#1185): the agent prompt first, then the source code alone. */
export type WebsiteView = "prompt" | "code";
export const WEBSITE_VIEWS: { value: WebsiteView; label: string }[] = [
  { value: "prompt", label: "Copy agent prompt" },
  { value: "code", label: "Source code" },
];
export const DEFAULT_WEBSITE_VIEW: WebsiteView = "prompt";

/** The agent prompt with the key the page holds put in it; without one, as Lake wrote it (it says
 * how to make the key). Null when this Lake answers no prompt. */
export function agentPrompt(snippets: Pick<Snippets, "prompt" | "key"> | null | undefined, secret: string | null): string | null {
  if (!snippets?.prompt) return null;
  return withKey(snippets.prompt, snippets.key?.placeholder ?? "YOUR_LAKE_SEARCH_KEY", secret);
}

/** Whether the model step opens its Ask Orama conversation now (lake#1182): the search works, no
 * conversation is open, nothing is training or trained, and the balance lets a turn start. Opened
 * once per quickstart; the person never has to ask for it. */
export function opensConversation(status: Pick<Status, "quickstart_id" | "index" | "conversation" | "training" | "model"> | null | undefined, blocked: boolean): boolean {
  return !!status?.quickstart_id && !!status.index?.published && !status.conversation && !status.training && !status.model && !blocked;
}

/** Where what a confirmed proposal made lives (lake#1182), so the conversation can link it: a model
 * goes on in this page (null), the rest open in their own page. */
export function whereItLives(capability: string): { path: string; label: string } | null {
  if (capability === "train_quickstart") return null;
  if (/^(create|deploy|build|start|scale|restart)_app$/.test(capability)) return { path: "/ship/apps", label: "Ship → Apps" };
  if (capability === "discover" || capability.endsWith("_opportunities") || capability === "dismiss_opportunity") return { path: "/design/opportunities", label: "Design → Opportunities" };
  if (["generate_dataset", "upload_dataset", "create_dataset_from_query", "load_drive_dataset"].includes(capability)) return { path: "/train/data", label: "Data → Datasets" };
  if (["create_project", "compile_intent", "compose_solution", "run_solution", "create_pipeline", "start_run"].includes(capability)) return { path: "/design/projects", label: "Design → Projects" };
  if (capability === "train_model" || capability === "retrain_model") return { path: "/train/models", label: "Train → Models" };
  return null;
}

/** The model step's title: a conversation until something is training, then the model. */
export function modelStepTitle(status: Pick<Status, "training" | "model">): string {
  return status.training || status.model ? "Train a Model on the Same Data" : "Decide What to Build";
}
