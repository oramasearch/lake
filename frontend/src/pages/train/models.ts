/** What Train → Models and Ship → Models share about a model (lake#599), as pure functions the
 * tests run: which trained records are versions of one model, its held-out score, the score over
 * its versions, how two versions differ, the pipeline steps it belongs to, the `predict` call to
 * make, and its traffic.
 *
 * A model is a learned function. The workspace engine keeps one record per training: each run of
 * a pipeline's model step, or each trial of an experiment, makes a new record with its own id. The
 * records of one model are its versions: the same model name from the same pipeline of the same
 * project, or from the same experiment when no pipeline is named. A pipeline may hold several
 * models (several model steps, each named). */

export type Row = Record<string, any>;

function text(row: Row | null | undefined, key: string): string {
  const value = row?.[key];
  if (value === null || value === undefined) return "";
  if (typeof value === "string") return value;
  if (typeof value === "number" || typeof value === "boolean") return String(value);
  return "";
}

/* ------------------------------------------------------------------ scores */

/** The held-out figures a model can carry, the one a reader looks for first, and whether a
 * higher value is better. */
export const HELD_OUT: { key: string; label: string; higher: boolean }[] = [
  { key: "held_out_auc", label: "AUC", higher: true },
  { key: "holdout_auc", label: "AUC", higher: true },
  { key: "held_out_accuracy", label: "accuracy", higher: true },
  { key: "held_out_rmse", label: "RMSE", higher: false },
  { key: "held_out_mae", label: "MAE", higher: false },
  { key: "held_out_log_loss", label: "log loss", higher: false },
];

export type Score = { key: string; label: string; value: number; higher: boolean };

/** A record's held-out score: the first held-out figure it measured, or null when it measured
 * none (an in-sample figure is never a score). */
export function heldOutScore(model: Row | null | undefined, key?: string): Score | null {
  const metrics = (model?.metrics ?? {}) as Row;
  for (const figure of HELD_OUT) {
    if (key && figure.key !== key) continue;
    const value = metrics[figure.key];
    if (typeof value === "number" && Number.isFinite(value)) return { ...figure, value };
  }
  return null;
}

/** The in-sample AUC a record carries, said only as a warning. */
export function inSampleAuc(model: Row | null | undefined): number | null {
  const value = (model?.metrics ?? {}) as Row;
  return typeof value.in_sample_auc === "number" ? value.in_sample_auc : null;
}

/* ------------------------------------------------------------------ versions */

/** Which experiment each trial model belongs to (from `experimentRefs`), by model id. */
export type ExperimentOf = (model: Row) => { id: string; name: string } | undefined;

/** The key that makes records versions of one model. */
export function modelKey(model: Row, experimentOf?: ExperimentOf): string {
  const name = text(model, "name") || text(model, "kind") || "model";
  const pipeline = text(model, "pipeline_name");
  const project = text(model, "project") || text(model, "project_name");
  if (pipeline) return `pipeline\u0000${project}\u0000${pipeline}\u0000${name}`;
  const experiment = experimentOf?.(model);
  if (experiment?.id) return `experiment\u0000${experiment.id}\u0000${name}`;
  return `project\u0000${project}\u0000${name}`;
}

export type ModelGroup = {
  key: string;
  name: string;
  kind: string;
  project: string;
  projectName: string;
  pipeline: string;
  /** Newest first. */
  versions: Row[];
  latest: Row;
  /** The latest version's held-out score, else the newest version's that has one. */
  score: Score | null;
  /** The published versions, newest first. */
  published: Row[];
  runs: string[];
};

function created(model: Row): string {
  return text(model, "created");
}

/** Records as models, each with its versions newest first; the models ordered by their newest
 * version, newest first. */
export function groupModels(records: Row[], experimentOf?: ExperimentOf): ModelGroup[] {
  const groups = new Map<string, Row[]>();
  for (const model of records) {
    if (!model || typeof model !== "object") continue;
    const key = modelKey(model, experimentOf);
    const list = groups.get(key);
    if (list) list.push(model);
    else groups.set(key, [model]);
  }
  const out: ModelGroup[] = [];
  for (const [key, list] of groups) {
    const versions = [...list].sort((a, b) => created(b).localeCompare(created(a)) || text(b, "id").localeCompare(text(a, "id")));
    const latest = versions[0];
    const scored = versions.find((version) => heldOutScore(version) !== null);
    const runs: string[] = [];
    for (const version of versions) {
      const run = text(version, "run_id") || text(version, "run");
      if (run && !runs.includes(run)) runs.push(run);
    }
    out.push({
      key,
      name: text(latest, "name") || text(latest, "kind") || "model",
      kind: text(latest, "kind"),
      project: text(latest, "project"),
      projectName: text(latest, "project_name"),
      pipeline: text(latest, "pipeline_name"),
      versions,
      latest,
      score: heldOutScore(latest) ?? (scored ? heldOutScore(scored) : null),
      published: versions.filter((version) => version.exposed === true),
      runs,
    });
  }
  return out.sort((a, b) => created(b.latest).localeCompare(created(a.latest)));
}

/** The model a record id belongs to, or undefined. */
export function groupOf(groups: ModelGroup[], id: string): ModelGroup | undefined {
  return groups.find((group) => group.versions.some((version) => text(version, "id") === id));
}

/** "v3": a version's number, counted from the oldest. */
export function versionNumber(group: ModelGroup, id: string): number {
  const index = group.versions.findIndex((version) => text(version, "id") === id);
  return index < 0 ? 0 : group.versions.length - index;
}

/** One held-out figure over a model's versions, oldest first, for the score chart: each version's
 * training time (ms) and value. Versions that did not measure it are left out. */
export function scoreSeries(group: ModelGroup, key: string): { at: number; value: number; id: string }[] {
  const out: { at: number; value: number; id: string }[] = [];
  for (const version of [...group.versions].reverse()) {
    const score = heldOutScore(version, key);
    const at = Date.parse(created(version));
    if (score && Number.isFinite(at)) out.push({ at, value: score.value, id: text(version, "id") });
  }
  return out;
}

/** The held-out figures any of the versions measured, in HELD_OUT's order. */
export function measuredFigures(group: ModelGroup): Score[] {
  const out: Score[] = [];
  for (const figure of HELD_OUT) {
    if (out.some((seen) => seen.label === figure.label)) continue;
    const found = group.versions.map((version) => heldOutScore(version, figure.key)).find((score) => score !== null);
    if (found) out.push(found);
  }
  return out;
}

export type Change = { key: string; label: string; from: number | null; to: number | null; better: boolean | null };

/** How `to` differs from `from` on every number either measured: the held-out figures first,
 * each said better or worse, then the rest as they are. */
export function compareVersions(from: Row, to: Row): Change[] {
  const a = (from?.metrics ?? {}) as Row;
  const b = (to?.metrics ?? {}) as Row;
  const keys = [...new Set([...Object.keys(a), ...Object.keys(b)])].filter((key) => typeof a[key] === "number" || typeof b[key] === "number");
  const rank = (key: string) => {
    const index = HELD_OUT.findIndex((figure) => figure.key === key);
    return index < 0 ? HELD_OUT.length : index;
  };
  keys.sort((x, y) => rank(x) - rank(y) || x.localeCompare(y));
  return keys.map((key) => {
    const figure = HELD_OUT.find((f) => f.key === key);
    const before = typeof a[key] === "number" ? (a[key] as number) : null;
    const after = typeof b[key] === "number" ? (b[key] as number) : null;
    const better = figure && before !== null && after !== null && before !== after ? (after > before) === figure.higher : null;
    return { key, label: figure ? `Held-out ${figure.label}` : key, from: before, to: after, better };
  });
}

/** The features a model reads, from its feature schema. */
export function featuresOf(model: Row | null | undefined): string[] {
  const schema = (model?.feature_schema ?? {}) as Row;
  const features = Array.isArray(schema.features) ? schema.features : [];
  return features.map((feature: unknown) => (typeof feature === "string" ? feature : feature && typeof feature === "object" ? text(feature as Row, "name") || JSON.stringify(feature) : String(feature))).filter(Boolean);
}

/** What it learned from, when the model says: its feature schema's dataset. The run report says
 * it for every model (Train → Model reads it there). */
export function datasetOf(model: Row | null | undefined): string {
  const dataset = ((model?.feature_schema ?? {}) as Row).dataset;
  if (typeof dataset === "string") return dataset;
  if (dataset && typeof dataset === "object") return text(dataset as Row, "path") || text(dataset as Row, "source") || text(dataset as Row, "name");
  return "";
}

/* ------------------------------------------------------------------ pipeline steps */

export type PipelineStep = { id: string; kind: string; name: string; attributes: Row };

/** The model steps of a pipeline (get_graph's nodes of kind `model`) this model belongs to: the
 * step named as the model, or the step whose `model_name` attribute is, else every model step
 * (`exact` false: the pipeline does not say which one trained it). */
export function modelSteps(graph: Row | null | undefined, modelName: string): { steps: PipelineStep[]; exact: boolean } {
  const nodes = Array.isArray(graph?.nodes) ? (graph!.nodes as Row[]) : [];
  const steps: PipelineStep[] = nodes
    .filter((node) => node && typeof node === "object" && text(node, "kind") === "model")
    .map((node) => ({ id: text(node, "id"), kind: "model", name: text(node, "name"), attributes: node.attributes && typeof node.attributes === "object" ? (node.attributes as Row) : {} }));
  const named = steps.filter((step) => step.name === modelName || text(step.attributes, "model_name") === modelName || step.id === modelName);
  if (named.length) return { steps: named, exact: true };
  return { steps, exact: steps.length === 1 };
}

/* ------------------------------------------------------------------ calling it */

/** One example row for a model: its key in `entity` and each feature at 0. */
export function sampleRow(model: Row | null | undefined): Row {
  const row: Row = { entity: "example-1" };
  for (const feature of featuresOf(model)) row[feature] = 0;
  return row;
}

/** The `predict` call as `curl`, the account, workspace and model filled in. */
export function predictCurl(origin: string, customerId: string, workspaceId: string, model: Row, rows: Row[] = [sampleRow(model)]): string {
  const body = { customer_id: customerId, workspace_id: workspaceId || "<workspace id>", model_id: text(model, "id"), rows };
  return `curl -s ${origin}/v1/predict \\\n  -H "Authorization: Bearer $LAKE_KEY" -H 'Content-Type: application/json' \\\n  -d '${JSON.stringify(body)}'`;
}

/** Rows typed into the try-it panel: one JSON object or a list of them. A message when it is
 * not. */
export function parseRows(input: string): { rows: Row[] } | { error: string } {
  let value: unknown;
  try {
    value = JSON.parse(input);
  } catch {
    return { error: "The rows are not JSON." };
  }
  const rows = Array.isArray(value) ? value : [value];
  if (!rows.length) return { error: "Send at least one row." };
  if (rows.some((row) => !row || typeof row !== "object" || Array.isArray(row))) return { error: "Each row is a JSON object keyed by the model's features." };
  return { rows: rows as Row[] };
}

/** The account's keys that may call `predict`: unrevoked tokens scoped to it, or to everything. */
export function predictKeys(sessions: Row[] | undefined): Row[] {
  return (sessions ?? []).filter((row) => (row.kind === "api_token" || row.kind === "static") && !row.revoked_at && Array.isArray(row.scopes) && row.scopes.some((scope: unknown) => scope === "predict" || scope === "*"));
}

/* ------------------------------------------------------------------ traffic */

export type Traffic = { calls: number; errors: number; clientErrors: number; serverErrors: number; meanMs: number | null; steps: { at: number; calls: number; errors: number; meanMs: number | null }[] };

const NONE: Traffic = { calls: 0, errors: 0, clientErrors: 0, serverErrors: 0, meanMs: null, steps: [] };

function count(value: unknown): number {
  return typeof value === "number" && Number.isFinite(value) ? value : 0;
}

/** One model's traffic from `model_traffic`'s answer; no traffic when it has none. */
export function trafficOf(answer: Row | null | undefined, modelId: string): Traffic {
  const entry = (Array.isArray(answer?.models) ? (answer!.models as Row[]) : []).find((model) => text(model, "model_id") === modelId);
  if (!entry) return NONE;
  return {
    calls: count(entry.calls),
    errors: count(entry.errors),
    clientErrors: count(entry.client_errors),
    serverErrors: count(entry.server_errors),
    meanMs: typeof entry.mean_ms === "number" ? entry.mean_ms : null,
    steps: (Array.isArray(entry.steps) ? (entry.steps as Row[]) : []).map((step) => ({
      at: count(step.at_ms),
      calls: count(step.calls),
      errors: count(step.errors),
      meanMs: typeof step.mean_ms === "number" ? step.mean_ms : null,
    })),
  };
}

/** "2.5%": errors as a share of calls, or "—" with no calls. */
export function errorRate(traffic: Traffic): string {
  if (!traffic.calls) return "—";
  const share = (100 * traffic.errors) / traffic.calls;
  return `${share < 10 ? share.toFixed(1) : share.toFixed(0)}%`;
}

/** The steps of a range with no calls filled in as zero, so a chart shows the quiet minutes too.
 * Steps start at `fromMs` rounded down to the minute, as the series counts them. */
export function filledSteps(traffic: Traffic, fromMs: number, toMs: number, stepMs: number): Traffic["steps"] {
  if (!(stepMs > 0) || !(toMs > fromMs)) return traffic.steps;
  const byAt = new Map(traffic.steps.map((step) => [step.at, step]));
  const out: Traffic["steps"] = [];
  const first = fromMs - (fromMs % 60_000);
  for (let at = first; at < toMs && out.length < 10_000; at += stepMs) out.push(byAt.get(at) ?? { at, calls: 0, errors: 0, meanMs: null });
  return out;
}
