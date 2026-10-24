/** The quickstart process as pure rules (lake#1128, docs/ui/quickstart.md), so `npm test` runs them
 * (tests/quickstart.test.ts): what a dropped file is sent as, the columns a first model trains on,
 * how often the page reads the status, when it publishes a trained model, the key put into the
 * website code, and whether the balance lets a paid step start. No React here. */

/** A step as quickstart_status answers it. */
export type StepState = "pending" | "running" | "done" | "failed";
export type Step = { step: string; title: string; state: StepState; error: { code: string; message: string } | null; progress?: { status?: string; started_at?: string } | null };
export type QuickstartState = "waiting_for_workspace" | "not_started" | "in_progress" | "search_ready" | "training" | "trained" | "done" | "failed";
export type Status = {
  quickstart_id: string | null;
  customer_id: string;
  workspace_id: string;
  state: QuickstartState;
  next: string;
  steps: Step[];
  dataset: { id: string; database: string; name: string; rows?: number | null; columns?: { name: string; type: string }[] } | null;
  index: { id: string; fields: string[]; published: boolean } | null;
  training: { run_id?: string; target?: string | null; entity_column?: string | null; methodology?: string } | null;
  model: { id: string; published: boolean } | null;
  workspace?: { ready?: boolean; phase?: string; message?: string };
  quickstarts?: { quickstart_id: string; dataset_id: string | null; created_at: string | null }[];
  costs?: Costs;
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
  key: { placeholder: string; scopes: string[]; create: { capability: string; input: { name: string; scopes: string[] } }; note: string } | null;
  next?: string;
};

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
  return state === "done" ? "success" : state === "running" ? "info" : state === "failed" ? "danger" : "neutral";
}

/** A search hit's row in one line, as the website snippet shows it. */
export function hitLine(fields: Record<string, unknown> | undefined): string {
  return Object.values(fields ?? {})
    .map((value) => (value === null || value === undefined ? "" : String(value)))
    .filter(Boolean)
    .join(" · ");
}
