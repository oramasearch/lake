/** The Design pages' rules, as pure functions: what lake's renderers (web_ui.cpp) and form
 * handlers (http_server.cpp) computed on the server, ported so the pages compute the same. No
 * imports, so `npm test` runs them directly. */

// eslint-disable-next-line @typescript-eslint/no-explicit-any
export type J = Record<string, any>;

/** A field as text: a string as it is, anything else as JSON, absent as "" (json_text). */
export function text(value: J | null | undefined, key: string): string {
  const v = value?.[key];
  if (v === undefined || v === null) return "";
  return typeof v === "string" ? v : JSON.stringify(v);
}

/** A table cell (cell_text): strings as they are, floats to 4 significant digits, else JSON. */
export function cellText(value: unknown): string {
  if (value === null || value === undefined) return "";
  if (typeof value === "string") return value;
  if (typeof value === "number") return Number.isInteger(value) ? String(value) : formatG4(value);
  return JSON.stringify(value);
}

/** C's `%.4g`: four significant digits, trailing zeros dropped, exponent outside 1e-4..1e4. */
export function formatG4(n: number): string {
  if (!Number.isFinite(n)) return String(n);
  if (n === 0) return "0";
  const exponent = Math.floor(Math.log10(Math.abs(n)));
  if (exponent < -4 || exponent >= 4) {
    const [mantissa, power] = n.toExponential(3).split("e");
    const trimmed = mantissa.includes(".") ? mantissa.replace(/\.?0+$/, "") : mantissa;
    const p = Number(power);
    return `${trimmed}e${p < 0 ? "-" : "+"}${String(Math.abs(p)).padStart(2, "0")}`;
  }
  const fixed = n.toFixed(Math.max(0, 3 - exponent));
  return fixed.includes(".") ? fixed.replace(/\.?0+$/, "") : fixed;
}

/** "1234567" → "1,234,567" (count_text). */
export function countText(n: number): string {
  return Math.trunc(n).toString().replace(/\B(?=(\d{3})+(?!\d))/g, ",");
}

/** A timestamp as lake printed it: "2026-09-27T13:29:22Z" → "2026-09-27 13:29 UTC" (when). */
export function whenUtc(timestamp: string | null | undefined): string {
  const t = timestamp ?? "";
  if (t.length < 16) return t;
  return `${t.slice(0, 10)} ${t.slice(11, 16)} UTC`;
}

/** A list as text: strings, or an object's `role` else `name` (joined). */
export function joined(list: unknown, separator = ", "): string {
  if (!Array.isArray(list)) return "";
  const out: string[] = [];
  for (const item of list) {
    const t =
      typeof item === "string"
        ? item
        : item && typeof item === "object"
          ? text(item as J, "role") || text(item as J, "name")
          : JSON.stringify(item);
    if (t) out.push(t);
  }
  return out.join(separator);
}

/** "Churn & Retention" → "churn-retention": a default project name (slug_text). */
export function slugText(value: string): string {
  let out = "";
  for (const c of value) {
    if (/^[A-Za-z0-9]$/.test(c)) out += c.toLowerCase();
    else if (out && !out.endsWith("-")) out += "-";
  }
  return out.replace(/-+$/, "");
}

/** A table the engine wrote as a run's output rather than data a person loaded (result_dataset,
 * datasets.cpp engine_database): kept out of every dataset picker. */
export function isResultDataset(dataset: J): boolean {
  if (dataset?.kind === "result") return true;
  const database = dataset?.database;
  if (typeof database !== "string") return false;
  return (database.length > "_results".length && database.endsWith("_results")) || database === "forecasts" || database === "feedback" || database === "system";
}

/** The datasets a picker offers, as `<database or "datasets">.<name>` with their names. */
export function datasetChoices(datasets: unknown): { path: string; name: string }[] {
  if (!Array.isArray(datasets)) return [];
  return datasets
    .filter((d) => d && typeof d === "object" && !isResultDataset(d))
    .map((d) => ({ path: `${text(d, "database") || "datasets"}.${text(d, "name")}`, name: text(d, "name") }));
}

/** The Project Agent's compile check graphs are lake's, not the project's pipelines (lake#438). */
export const PROJECT_AGENT_CHECK = "Project Agent check";
export function withoutProjectAgentChecks<T extends J>(graphs: unknown): T[] {
  if (!Array.isArray(graphs)) return [];
  return graphs.filter((g) => !(g && typeof g === "object" && typeof g.name === "string" && g.name.startsWith(PROJECT_AGENT_CHECK)));
}

/** A project's state from its pipelines (the Project Agent's check graphs left out): one approved
 * pipeline makes it "approved"; pipelines none of which is approved, "proposed"; none, "no pipelines". */
export type ProjectState = "approved" | "proposed" | "no pipelines";
export function projectState(graphs: J[]): { state: ProjectState; pipelines: number; approved: number } {
  const approved = graphs.filter((g) => text(g, "approval") === "approved").length;
  return { state: graphs.length === 0 ? "no pipelines" : approved > 0 ? "approved" : "proposed", pipelines: graphs.length, approved };
}
export const PROJECT_STATE_TONE: Record<ProjectState, "success" | "warning" | "neutral"> = { approved: "success", proposed: "warning", "no pipelines": "neutral" };

/** The status badge's tone of a methodology or solution (runs_badge_of). */
export function runsBadge(entry: J): { label: string; tone: "neutral" | "success" | "warning" } {
  if (text(entry, "status") === "sketch") return { label: "In design", tone: "neutral" };
  if (entry?.runnable === true) return { label: entry.partial === true ? "Runs, in part" : "Runs today", tone: "success" };
  return { label: "Not yet", tone: "warning" };
}

/** A run status badge's tone (run_status_badge); the label is the status itself, or "unknown". */
export function runStatusTone(status: string): "success" | "danger" | "warning" | "neutral" {
  if (status === "succeeded" || status === "pass" || status === "within bounds") return "success";
  if (status === "failed" || status === "fail" || status === "attention needed") return "danger";
  if (status === "warn" || status === "queued" || status === "pending" || status === "running") return "warning";
  return "neutral";
}

export function runIsActive(run: J): boolean {
  const s = text(run, "status");
  return s === "queued" || s === "pending" || s === "running";
}

/** A background agent task still working (get_agent_task's lowercase status, or a stored phase). */
export function taskIsActive(status: string | null | undefined): boolean {
  const s = (status ?? "").toLowerCase();
  return s === "pending" || s === "running";
}

/** A gate verdict's tone: pass green, warn amber, anything else red. */
export function verdictTone(verdict: string): "success" | "warning" | "danger" {
  return verdict === "pass" ? "success" : verdict === "warn" ? "warning" : "danger";
}

/** A gate check's tone: a check that did not apply is neither a pass nor a failure (zetta#1240). */
export function checkTone(status: string): "neutral" | "success" | "warning" | "danger" {
  return status === "not_applicable" ? "neutral" : verdictTone(status);
}

/** How many of a gate's checks applied and how many did not (zetta#1240). The engine counts both
 * (`applied`, `not_applicable`) and lists only the applied checks unless asked for an audit; an
 * older engine sends neither count, so the listed checks are counted. */
export function gateCounts(gate: J | null | undefined): { applied: number; notApplicable: number } {
  const checks: J[] = Array.isArray(gate?.checks) ? gate!.checks : [];
  const listedNa = checks.filter((c) => c?.status === "not_applicable").length;
  const count = (key: string, fallback: number) => (typeof gate?.[key] === "number" && Number.isFinite(gate[key]) ? (gate[key] as number) : fallback);
  return { applied: count("applied", checks.length - listedNa), notApplicable: count("not_applicable", listedNa) };
}

/** "1 check did not apply", "107 checks did not apply"; "" for none. */
export function notApplicableLine(count: number): string {
  if (count <= 0) return "";
  return `${count} ${count === 1 ? "check" : "checks"} did not apply`;
}

/** A dataset's columns from list_datasets, by the "<database>.<name>" path datasetChoices gives. */
export function datasetColumns(datasets: unknown, path: string): string[] {
  if (!Array.isArray(datasets) || !path) return [];
  const found = datasets.find((d) => d && typeof d === "object" && `${text(d, "database") || "datasets"}.${text(d, "name")}` === path);
  return Array.isArray(found?.columns) ? found.columns.filter((c: unknown): c is string => typeof c === "string") : [];
}

/** An id written anywhere in a text ("M-3", "M-3 Segmentation"), else "". */
export function idToken(value: string, prefix: "M" | "S"): string {
  for (let at = 0; at + 2 < value.length; ++at) {
    if (value[at] !== prefix || value[at + 1] !== "-") continue;
    let end = at + 2;
    while (end < value.length && /[0-9]/.test(value[end])) ++end;
    if (end > at + 2) return value.slice(at, end);
  }
  return "";
}

/** The methodology an Opportunity needs (find_methodology): the `M-n` in its methodology, else the
 * methodology that lists its catalogue solution `S-n`. Null when the engine has no registry. */
export function findMethodology(methodologies: unknown, methodology: string, solution: string): J | null {
  if (!Array.isArray(methodologies)) return null;
  const m = idToken(methodology, "M");
  if (m) {
    const found = methodologies.find((e) => text(e, "id") === m);
    if (found) return found;
  }
  const s = idToken(solution, "S");
  if (s) {
    const found = methodologies.find((e) => Array.isArray(e?.solutions) && e.solutions.includes(s));
    if (found) return found;
  }
  return null;
}

/** The methodology ids a solution composes, with their minimum level ("methodologies" or
 * "composition": strings, or objects naming `methodology` else `id`). */
export function solutionMethodologies(solution: J): { id: string; level: number }[] {
  const list = Array.isArray(solution?.methodologies) ? solution.methodologies : Array.isArray(solution?.composition) ? solution.composition : [];
  const out: { id: string; level: number }[] = [];
  for (const item of list) {
    const id = typeof item === "string" ? item : text(item, "methodology") || text(item, "id");
    if (!id) continue;
    const level = item && typeof item === "object" && typeof item.minimum_level === "number" ? item.minimum_level : 1;
    out.push({ id, level });
  }
  return out;
}

/** Whether a solution uses a methodology: the server checked "methodologies" and "composition". */
export function solutionUses(solution: J, id: string): boolean {
  for (const key of ["methodologies", "composition"]) {
    const list = solution?.[key];
    if (!Array.isArray(list)) continue;
    if (list.some((item) => (typeof item === "string" ? item : text(item, "methodology") || text(item, "id")) === id)) return true;
  }
  return false;
}

/** A Project Agent problem or failure as one plain sentence (plain_agent_failure, lake#438). */
export function plainAgentFailure(input: string): string {
  let t = input;
  let at = t.indexOf(" did not succeed");
  if (at < 0) at = t.indexOf(" FAILED:");
  if (at >= 0) {
    let call = t.slice(0, at);
    const space = call.lastIndexOf(" ");
    if (space >= 0) call = call.slice(space + 1);
    return `The Project Agent's ${call} check did not succeed on your workspace.`;
  }
  const brace = t.indexOf("{");
  const bracket = t.indexOf("[");
  const cuts = [brace, bracket].filter((i) => i >= 0);
  if (cuts.length) t = t.slice(0, Math.min(...cuts));
  t = t.replace(/[ :,\n.]+$/, "");
  if (!t) return "The Project Agent's check did not succeed on your workspace.";
  return `${t}.`;
}

/** The foundation a solution task proposed, with the person's delivery and economics and the
 * dataset's source and table merged in (proposed_solution_foundation). */
export function proposedFoundation(task: J | null | undefined): J {
  const result = task?.answer?.result;
  const foundation = result && typeof result === "object" ? result.foundation : undefined;
  if (!foundation || typeof foundation !== "object" || Array.isArray(foundation)) return {};
  const proposal: J = { ...foundation };
  const context = task?.context && typeof task.context === "object" ? task.context : {};
  if ("delivery" in context) proposal.delivery = context.delivery;
  if ("roi_model" in context) proposal.roi_model = context.roi_model;
  const sources: J = proposal.sources && typeof proposal.sources === "object" && !Array.isArray(proposal.sources) ? { ...proposal.sources } : {};
  const dataset = typeof context.dataset === "string" ? context.dataset : "";
  if (dataset) {
    const dot = dataset.indexOf(".");
    sources.source = dot < 0 ? "datasets" : dataset.slice(0, dot);
    sources.table = dot < 0 ? dataset : dataset.slice(dot + 1);
  }
  proposal.sources = sources;
  return proposal;
}

/** The exclusion predicates a solution's dataset lacks (missing_solution_predicates): what the
 * proposal says, and for S-1 the three it must name. */
export function missingPredicates(task: J | null | undefined, proposal: J): string[] {
  const missing: unknown[] = Array.isArray(proposal.missing_predicates) ? [...proposal.missing_predicates] : [];
  if (task?.context?.solution === "S-1") {
    const predicates = proposal.predicates && typeof proposal.predicates === "object" ? proposal.predicates : {};
    for (const name of ["in_collections", "open_complaint", "vulnerable"]) {
      const named = typeof predicates[name] === "string" && predicates[name] !== "";
      if (!named && !missing.includes(name)) missing.push(name);
    }
  }
  return missing.map((m) => (typeof m === "string" ? m : JSON.stringify(m)));
}

/** A solution project found from its graphs: the first whose manifest node carries a foundation. */
export type SolutionProject = {
  graph: string;
  graphId: string;
  foundation: J;
  sources: J;
  stages: { id: string; name: string; kind: string; stage: unknown }[];
};

export function solutionProjectOf(details: { summary: J; detail: J | undefined }[]): SolutionProject | null {
  for (const { summary, detail } of details) {
    if (!detail) continue;
    let foundation: J | null = null;
    let sources: J = {};
    const stages: SolutionProject["stages"] = [];
    for (const node of Array.isArray(detail.nodes) ? detail.nodes : []) {
      const attributes = node?.attributes && typeof node.attributes === "object" ? node.attributes : {};
      if (text(node, "id") === "solution_manifest" && attributes.foundation && typeof attributes.foundation === "object" && !Array.isArray(attributes.foundation)) {
        foundation = attributes.foundation;
        sources = attributes.sources && typeof attributes.sources === "object" ? attributes.sources : {};
      }
      if ("solution_stage" in attributes) {
        stages.push({ id: text(node, "id"), name: text(node, "name"), kind: text(node, "kind"), stage: attributes.solution_stage });
      }
    }
    if (!foundation) continue;
    return { graph: text(summary, "name"), graphId: text(detail, "graph_id") || text(summary, "id"), foundation, sources, stages };
  }
  return null;
}

/** The runs of one pipeline (by name or graph id), newest first. */
export function runsOf(runs: unknown, name: string, graphId: string): J[] {
  if (!Array.isArray(runs)) return [];
  return runs.filter((r) => text(r, "graph") === name || (!!graphId && text(r, "graph") === graphId)).sort((a, b) => (text(b, "created") > text(a, "created") ? 1 : text(b, "created") < text(a, "created") ? -1 : 0));
}

/** The table a run wrote its decisions to, when it is safe to name in SQL. */
export function decisionsRef(run: J | null | undefined): string {
  let ref = "";
  for (const output of Array.isArray(run?.outputs) ? run!.outputs : []) {
    if (text(output, "name") === "solution decisions") ref = text(output, "ref");
  }
  return ref && /^[A-Za-z0-9_.]+$/.test(ref) ? ref : "";
}

/** The summary line of a solution project (solution_state_summary). */
export function solutionState(runs: J[]): { lead: string; rest: string; failed: boolean } {
  if (!runs.length) return { lead: "Created.", rest: "The solution is ready to run.", failed: false };
  const latest = runs[0];
  const status = text(latest, "status");
  const stage = text(latest, "stage");
  if (runIsActive(latest)) return { lead: "Running.", rest: stage ? `Reached ${stage}.` : "Preparing the solution.", failed: false };
  if (status === "failed") {
    const error = text(latest, "error");
    return { lead: `Last run failed${stage ? ` at ${stage}` : ""}.`, rest: error, failed: true };
  }
  if (status === "succeeded") return { lead: "Delivering.", rest: "The last run succeeded.", failed: false };
  return { lead: "Created.", rest: "No run has completed yet.", failed: false };
}

/** The policy's current configuration: its latest version's, else its own. */
export function policyConfig(policy: J): J {
  let config: J = policy?.config && typeof policy.config === "object" ? policy.config : {};
  const versions = policy?.versions;
  if (Array.isArray(versions) && versions.length && versions[versions.length - 1] && typeof versions[versions.length - 1] === "object") {
    const last = versions[versions.length - 1];
    if ("config" in last) config = last.config;
  }
  return config;
}

/** The experiment's allocations, as experiment_summary derived them. */
export function experimentAllocations(experiment: J): J[] {
  const balance = experiment?.balance && typeof experiment.balance === "object" && !Array.isArray(experiment.balance) ? experiment.balance : {};
  let allocations: J[] = Array.isArray(balance.allocations) ? balance.allocations : [];
  if (!allocations.length && Array.isArray(experiment?.allocations)) allocations = experiment.allocations;
  if (!allocations.length && experiment && "holdout_fraction" in experiment) {
    const holdout = typeof experiment.holdout_fraction === "number" ? experiment.holdout_fraction : 0;
    allocations = [
      { name: "holdout", tier: "all", fraction: holdout },
      { name: "delivery", tier: "all", fraction: 1 - holdout },
    ];
  }
  return allocations;
}

export function allocationShare(allocation: J): string {
  const f = typeof allocation.configured_fraction === "number" ? allocation.configured_fraction : typeof allocation.fraction === "number" ? allocation.fraction : 0;
  return `${(f * 100).toFixed(1)}%`;
}

/** Whether the monitor is within bounds: no breach and no calibration breach. */
export function monitorHealthy(monitor: J): boolean {
  const breaches = Array.isArray(monitor?.breaches) ? monitor.breaches : [];
  return breaches.length === 0 && !(monitor?.calibration && monitor.calibration.breached === true);
}

/** The ledger's action entries in the solution's scope, newest first (solution_ledger_entries). */
export function ledgerEntries(ledger: J | undefined, scope: string): J[] {
  const entries = ledger?.entries;
  if (!Array.isArray(entries) || !scope) return [];
  const inScope = (cell: string) => cell === scope || (cell.startsWith(scope) && cell.length > scope.length && cell[scope.length] === "/");
  return entries
    .filter((e) => text(e, "kind") === "action" && inScope(text(e, "cell")))
    .sort((a, b) => (text(b, "recorded_at") > text(a, "recorded_at") ? 1 : text(b, "recorded_at") < text(a, "recorded_at") ? -1 : 0));
}

export function actionCounts(entries: J[]): { decisions: number; actions: number; held: number } {
  const counts = { decisions: 0, actions: 0, held: 0 };
  for (const e of entries) {
    const action = text(e, "action");
    if (action === "publish_driver_report") continue;
    counts.decisions += 1;
    if (action === "no_contact") counts.held += 1;
    else counts.actions += 1;
  }
  return counts;
}

/** The ten latest delivered actions (no `no_contact`, no driver report). */
export function latestActions(entries: J[]): J[] {
  return entries.filter((e) => text(e, "action") !== "no_contact" && text(e, "action") !== "publish_driver_report").slice(0, 10);
}

export function pendingA1(decision: J): boolean {
  return text(decision, "autonomy") === "A1" && text(decision, "status") === "pending_approval";
}

export type QueueRow = { key: string; entity: string; proposed: string; cell: string; score: string; governance: string; decisionId: string | null };

/** The A1 queue (a1_queue_content): pending A1 decisions first, by id, then held `no_contact`
 * entries whose decision is not pending; ten at most. */
export function a1Queue(entries: J[], decisions: unknown): QueueRow[] {
  const byId = new Map<string, J>();
  if (Array.isArray(decisions)) for (const d of decisions) byId.set(text(d, "id"), d);
  const ids = [...byId.keys()].sort();
  const held = new Map<string, J>();
  for (const e of entries) if (text(e, "action") === "no_contact") held.set(text(e, "decision"), e);
  const row = (entry: J | undefined, decision: J | undefined, key: string): QueueRow => {
    const pending = !!decision && pendingA1(decision);
    const scored = entry ?? decision!;
    return {
      key,
      entity: entry ? text(entry, "subject") : text(decision!, "entity_id"),
      proposed: decision ? text(decision, "action") : "no_contact",
      cell: entry ? text(entry, "cell") : text(decision!, "cell"),
      score: scored && "score" in scored ? cellText(scored.score) : "—",
      governance: pending ? "A1 · pending approval" : "Held · no action delivered",
      decisionId: pending ? text(decision!, "id") : null,
    };
  };
  const out: QueueRow[] = [];
  for (const id of ids) {
    const decision = byId.get(id)!;
    if (!pendingA1(decision)) continue;
    out.push(row(held.get(id), decision, `d:${id}`));
    if (out.length === 10) break;
  }
  entries.forEach((entry, index) => {
    if (out.length === 10 || text(entry, "action") !== "no_contact") return;
    const decision = byId.get(text(entry, "decision"));
    if (decision && pendingA1(decision)) return;
    out.push(row(entry, decision, `e:${index}`));
  });
  return out;
}

/** Whether a correctness check applies to this solution (certificate_check_applies). */
export function checkApplies(check: J, solution: string): boolean {
  const status = text(check, "status");
  const reason = text(check, "reason");
  const name = text(check, "name");
  if (status !== "pass") return true;
  if (reason.startsWith("no bound ") || reason.startsWith("no step ") || reason.startsWith("no ML step ") || reason.startsWith("no model step ")) return false;
  const prefix = solution.replace(/[^A-Za-z0-9]/g, "").toLowerCase();
  return name.includes(prefix + "_") || name.endsWith("/non_empty_graph") || name === "non_empty_graph";
}

/** The group a check belongs to: the part before "/", "S-n"/"B-n" from "s12_…", else Pipeline. */
export function checkGroup(name: string): string {
  const slash = name.indexOf("/");
  if (slash >= 0) return name.slice(0, slash);
  const underscore = name.indexOf("_");
  if (underscore > 1 && (name[0] === "s" || name[0] === "b") && /^[0-9]+$/.test(name.slice(1, underscore))) {
    return `${name[0].toUpperCase()}-${name.slice(1, underscore)}`;
  }
  return "Pipeline";
}

/** The certificate's checks grouped (std::map: groups sorted by name), and the hidden ones. */
export function certificateGroups(certificate: J, solution: string): { groups: [string, J[]][]; hidden: J[] } {
  const groups = new Map<string, J[]>();
  const hidden: J[] = [];
  for (const check of Array.isArray(certificate?.checks) ? certificate.checks : []) {
    if (!checkApplies(check, solution)) {
      hidden.push(check);
      continue;
    }
    const group = checkGroup(text(check, "name"));
    groups.set(group, [...(groups.get(group) ?? []), check]);
  }
  return { groups: [...groups.entries()].sort(([a], [b]) => (a < b ? -1 : a > b ? 1 : 0)), hidden };
}

/** strtod-like: the whole text is one number (leading spaces allowed, nothing after). */
export function parseNumber(value: string): number | null {
  if (!/^\s*[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?$/.test(value)) return null;
  const n = Number(value);
  return Number.isFinite(n) ? n : null;
}

/** from_chars-like: the whole text is one integer. */
export function parseInteger(value: string): number | null {
  if (!/^-?\d+$/.test(value)) return null;
  const n = Number(value);
  return Number.isSafeInteger(n) ? n : null;
}

/** Where Start your workspace lands (serve_workspace_form "start"): the section's list page when it
 * is one of these, else Projects. Transcripts (lake#561) is the app's own addition. */
export const START_RETURNS = ["/design/projects", "/design/agents", "/train/data", "/train/agents", "/train/runs", "/train/models", "/ship/models", "/ship/indexes", "/transcripts"];

/** A workspace page's list page (the server's workspace_sections `path`). */
const LIST_OF: [RegExp, string][] = [
  [/^\/design\/(projects|project|pipeline)$/, "/design/projects"],
  [/^\/design\/(opportunities|discover)$/, "/design/opportunities"],
  [/^\/design\/solutions?$/, "/design/solutions"],
  [/^\/design\/methodolog(y|ies)$/, "/design/methodologies"],
  [/^\/design\/(agents?|agent-library)(\/.*)?$/, "/design/agents"],
  [/^\/train\/agents?$/, "/train/agents"],
  [/^\/train\/(data|dataset|query)$/, "/train/data"],
  [/^\/train\/runs?$/, "/train/runs"],
  [/^\/train\/models?$/, "/train/models"],
  [/^\/ship\/models?$/, "/ship/models"],
  [/^\/ship\/index(es)?$/, "/ship/indexes"],
  [/^\/ship\/apps?$/, "/ship/apps"],
  [/^\/transcripts?$/, "/transcripts"],
];

export function startReturn(localPath: string): string {
  const list = LIST_OF.find(([pattern]) => pattern.test(localPath))?.[1] ?? "";
  return START_RETURNS.includes(list) ? list : "/design/projects";
}

/** Opportunities grouped as the page shows them (discover_content): the newest scan's task, its
 * new rows, earlier new rows, made into projects, not yet (latest scan only), dismissed. */
export function groupOpportunities(rows: J[]) {
  let lastScan = "";
  let latestTask = "";
  for (const row of rows) {
    if (text(row, "created_at") > lastScan) {
      lastScan = text(row, "created_at");
      latestTask = text(row, "task_id");
    }
  }
  const fresh: J[] = [];
  const earlier: J[] = [];
  const made: J[] = [];
  const later: J[] = [];
  const dismissed: J[] = [];
  for (const row of rows) {
    const state = text(row, "status");
    const current = text(row, "task_id") === latestTask;
    if (state === "new") (current ? fresh : earlier).push(row);
    else if (state === "project") made.push(row);
    else if (state === "not_yet" && current) later.push(row);
    else if (state === "dismissed") dismissed.push(row);
  }
  return { lastScan, latestTask, fresh, earlier, made, later, dismissed };
}

/** The workspace's newest scan as Opportunities shows it (list_opportunities' `last_scan`,
 * lake#1195): whoever started it and wherever, so the page shows a running scan and then its
 * results with no task id in the address. No scan yet is not scanning. */
export function scanState(lastScan: J | null | undefined) {
  const taskId = text(lastScan, "task_id");
  const status = taskId ? text(lastScan, "status") : "";
  return {
    taskId,
    status,
    scanning: taskIsActive(status),
    failed: status === "failed",
    startedAt: text(lastScan, "started_at"),
    finishedAt: text(lastScan, "finished_at"),
    message: text(lastScan, "message"),
  };
}

/** How often Opportunities reads again: every 3 seconds while its latest scan runs, else never. */
export function scanPollInterval(listed: J | null | undefined): number | false {
  return scanState(listed?.last_scan).scanning ? 3000 : false;
}

/** What the pipeline page says while a pipeline is proposed, beside Check (lake#813). */
export const APPROVE_FIRST = "Approve the pipeline first; checks run on an approved pipeline.";

/** An action's error in the page's words (lake#813). The engine refuses to check, run or deploy a
 * proposed pipeline with `not_approved` and a message naming the capability that approves it
 * ("approve it (workspace_act with capability approve_plan) before verification"); a page never
 * names a capability, so that refusal reads as {@link APPROVE_FIRST}. Anything else is unchanged. */
export function pipelineActionError(error: unknown): unknown {
  if (!error || typeof error !== "object") return error;
  const code = (error as { code?: unknown }).code;
  const message = (error as { message?: unknown }).message;
  const refusedAsProposed = code === "not_approved" || (typeof message === "string" && /\bapprove_plan\b/.test(message));
  return refusedAsProposed ? new Error(APPROVE_FIRST) : error;
}

/** The pipeline page's actions the engine refuses on a proposed pipeline: Check, Check and run,
 * Deploy, and the checks panel's "List them for an audit" (each calls `gate`). */
export const WAITS_FOR_APPROVAL = ["check", "run", "deploy", "audit"] as const;

/** After a successful Approve, a refusal shown before it no longer holds: clear every action in
 * {@link WAITS_FOR_APPROVAL} (lake#813). */
export function clearRefusalsOnApprove(actions: Record<(typeof WAITS_FOR_APPROVAL)[number], { setError: (error: unknown) => void }>): void {
  for (const name of WAITS_FOR_APPROVAL) actions[name].setError(null);
}

// lake#816: the words a pipeline title leaves out. The same lists as `pipeline_title` in lake's
// src/api/workspace_capabilities.cpp, which names a pipeline built without a name.
const TITLE_REQUEST = new Set(
  "predict forecast estimate classify detect find identify flag score rank recommend answer determine tell show spot list segment cluster group measure track explain know understand see figure work build create make help out me us all any each every".split(" "),
);
const TITLE_HELPER = new Set("which what whether who whom how that the a an will would could should can may might is are was were be been being likely unlikely going to do does did has have had most more least our my their its your".split(" "));
const TITLE_ENDING = new Set(
  "in within over during for by from with without using based before after and or but so because since per at on as than given when where if via into across among between until while about".split(" "),
);

// Typographic punctuation read as its plain form before the words are split, as lake's
// `pipeline_title` reads it: a dash ends the clause, curly quotes are quotes (an apostrophe inside a
// word stays one), an ellipsis is three dots and a no-break space is a space.
const TITLE_PLAIN: [RegExp, string][] = [
  [/[—–]/g, ","],
  [/[‘’‚‛]/g, "'"],
  [/[“”„‟«»]/g, '"'],
  [/…/g, "..."],
  [/ /g, " "],
];

/** Lowercase of ASCII letters only, as lake's `pipeline_title` folds a word to match its lists. */
function asciiLower(value: string): string {
  return value.replace(/[A-Z]/g, (c) => c.toLowerCase());
}

/** The capital of a lowercase letter of Latin-1, Latin Extended-A, Greek or Cyrillic; any other code
 * point as it is. The same letters as `title_capital` in lake's workspace_capabilities.cpp, rather
 * than `toUpperCase`, which maps more (ß is "SS") and would make the two titles differ. */
export function titleCapital(code: number): number {
  if (code >= 0x61 && code <= 0x7a) return code - 0x20;
  if (code >= 0xe0 && code <= 0xfe && code !== 0xf7) return code - 0x20;
  if (code === 0xff) return 0x178;
  if ((code >= 0x100 && code <= 0x137) || (code >= 0x14a && code <= 0x177)) return code % 2 === 1 ? code - 1 : code;
  if ((code >= 0x139 && code <= 0x148) || (code >= 0x179 && code <= 0x17e)) return code % 2 === 0 ? code - 1 : code;
  if (code >= 0x3b1 && code <= 0x3c9) return code === 0x3c2 ? 0x3a3 : code - 0x20;
  if (code >= 0x430 && code <= 0x44f) return code - 0x20;
  if (code >= 0x450 && code <= 0x45f) return code - 0x50;
  return code;
}

/** A short, readable pipeline title from its description (lake#816): the first clause, without a
 * leading request ("Predict which", "Build a"), helper words ("will", "likely to") or what follows a
 * preposition, at most four words and 48 characters, first letter capitalized. "Predict which
 * customers will churn (the churned column), with the reasons" is "Customers churn"; "Predict:
 * customer churn" is "Customer churn". Lake's `pipeline_title` follows the same rules for a
 * pipeline built through the API without a name; frontend/tests/pipeline-titles.json holds the cases
 * both are tested on. */
export function pipelineTitle(description: string): string {
  let text = description;
  for (const [from, to] of TITLE_PLAIN) text = text.replace(from, to);
  const isWord = (c: string | undefined) => c !== undefined && (/^[A-Za-z0-9_]$/.test(c) || c.codePointAt(0)! >= 0x80);
  // A word the title keeps: not a request, a helper or a leading "of".
  const carries = (each: string) => {
    const folded = asciiLower(each);
    return !TITLE_REQUEST.has(folded) && !TITLE_HELPER.has(folded) && folded !== "of";
  };
  const chars = [...text];
  const words: string[] = [];
  let word = "";
  for (let at = 0; at < chars.length; at++) {
    const c = chars[at];
    const inside = word !== "" && isWord(chars[at + 1]);
    if (isWord(c) || ((c === "-" || c === "'") && inside)) {
      word += c;
      continue;
    }
    if (word) {
      words.push(word);
      word = "";
    }
    const sentenceEnd = (c === "." || c === "!" || c === "?") && !isWord(chars[at + 1]);
    // The clause ends only once a word the title keeps is read, so "Predict: customer churn" reads
    // past the colon.
    if (("([{,;:\n".includes(c) || sentenceEnd || (c === "-" && !inside)) && words.some(carries)) break;
  }
  if (word) words.push(word);
  let kept: string[] = [];
  let leading = true;
  for (const each of words) {
    const folded = asciiLower(each);
    if (leading && TITLE_REQUEST.has(folded)) continue;
    if (TITLE_HELPER.has(folded)) continue;
    if (TITLE_ENDING.has(folded) && kept.length) break;
    if (folded === "of" && !kept.length) continue;
    leading = false;
    kept.push(each);
    if (kept.length === 4) break;
  }
  while (kept.length && asciiLower(kept[kept.length - 1]) === "of") kept.pop();
  if (!kept.length) kept = words.slice(0, 4);
  // At most 48 characters (code points, not UTF-16 units): whole words, or the first word cut.
  let title: string[] = [];
  for (const each of kept) {
    const letters = [...each];
    if (!title.length) {
      title = letters.slice(0, 48);
      continue;
    }
    if (title.length + 1 + letters.length > 48) break;
    title.push(" ", ...letters);
  }
  if (!title.length) return "Pipeline";
  title[0] = String.fromCodePoint(titleCapital(title[0].codePointAt(0)!));
  return title.join("");
}

/** `name`, numbered " (2)", " (3)" past the names already taken, as lake and the engine number a
 * pipeline's name. */
export function uniquePipelineName(name: string, taken: string[]): string {
  const used = new Set(taken);
  let candidate = name;
  for (let copy = 2; used.has(candidate); copy++) candidate = `${name} (${copy})`;
  return candidate;
}

/** The model name proposed for a pipeline (lake#816): its short title in lowercase words joined
 * by `_`, ending `_model`, at most about 40 characters: "Customers churn" is
 * `customers_churn_model`. A long pipeline name is shortened as {@link pipelineTitle} shortens a
 * description. */
export function defaultModelName(graph: string): string {
  let slug = pipelineTitle(graph)
    .toLowerCase()
    .replace(/[^a-z0-9]+/g, "_")
    .replace(/^_+|_+$/g, "");
  while (slug.length > 32 && slug.includes("_")) slug = slug.slice(0, slug.lastIndexOf("_"));
  slug = slug.slice(0, 32) || "pipeline";
  return slug === "model" || slug.endsWith("_model") ? slug : `${slug}_model`;
}

/** The name of the model a pipeline already trained, the newest when it trained several, else "".
 * The Run form keeps it, so running the pipeline again adds a version to that model rather than
 * starting another (lake#816): a pipeline trained before lake#816 keeps its `<project>_<pipeline>`. */
export function pipelineModelName(models: J[] | undefined, project: string, pipeline: string): string {
  if (!pipeline) return "";
  const own = (Array.isArray(models) ? models : [])
    .filter((model) => model && typeof model === "object" && text(model, "name"))
    .filter((model) => text(model, "pipeline_name") === pipeline && (text(model, "project_name") === project || text(model, "project") === project));
  own.sort((a, b) => text(b, "created").localeCompare(text(a, "created")));
  return own.length ? text(own[0], "name") : "";
}

/** The model name the Run form proposes for a pipeline (lake#816): the model it already trained
 * ({@link pipelineModelName}), else the short default from its title, numbered past another
 * pipeline's model. */
export function runModelName(models: J[] | undefined, project: string, pipeline: string): string {
  return pipelineModelName(models, project, pipeline) || uniqueModelName(defaultModelName(pipeline), models, project, pipeline);
}

/** The model name Build a Pipeline suggests for the pipeline lake will build under `name`: a name
 * the project already has is numbered " (2)" by lake, so the suggestion is that new pipeline's,
 * never the model of the pipeline that has the name (lake#816). */
export function buildModelName(name: string, taken: string[], models: J[] | undefined, project: string): string {
  const built = name.trim() ? uniquePipelineName(name.trim(), taken) : "";
  return built ? uniqueModelName(defaultModelName(built), models, project, built) : "";
}

/** `name`, or `name_2`, `name_3`… when a model by that name came from another pipeline (lake#816).
 * A model's own pipeline training again makes a new version of it, so its own name is not taken. */
export function uniqueModelName(name: string, models: J[] | undefined, project: string, pipeline: string): string {
  const taken = new Set(
    (Array.isArray(models) ? models : [])
      .filter((model) => model && typeof model === "object")
      .filter((model) => !(text(model, "pipeline_name") === pipeline && pipeline !== "" && (text(model, "project_name") === project || text(model, "project") === project)))
      .map((model) => text(model, "name")),
  );
  let candidate = name;
  for (let copy = 2; taken.has(candidate); copy++) candidate = `${name}_${copy}`;
  return candidate;
}

/** Whether a finished Project Agent pipeline turn asks the person something (agent_task_needs_reply
 * for agent-project): it succeeded, and it is a question or carries no proposal. */
export function projectAgentNeedsReply(task: J): boolean {
  if (text(task, "status") !== "succeeded") return false;
  const result = task?.answer?.result && typeof task.answer.result === "object" ? task.answer.result : {};
  if (text(result, "kind") === "question") return true;
  return !(result.proposal && typeof result.proposal === "object" && !Array.isArray(result.proposal));
}

/* ------------------------------------------------------------------ the pipeline graph */

export const STEP_KINDS = ["connect", "clean", "model", "index", "serve", "act"] as const;
export const EDGE_KINDS = ["data_flow", "control"] as const;

export type Step = { id: string; kind: string; name: string; attributes: J };
export type Edge = { from: string; to: string; kind: string };
export type Graph = { graph_id: string; name: string; project: string; approval: string; approved_at: string; steps: Step[]; edges: Edge[] };

/** get_graph's value as a pipeline (pipeline::from_json), or null when it cannot be read. */
export function graphOf(value: unknown): Graph | null {
  if (!value || typeof value !== "object" || Array.isArray(value)) return null;
  const v = value as J;
  const steps: Step[] = [];
  for (const node of Array.isArray(v.nodes) ? v.nodes : []) {
    if (!node || typeof node !== "object") return null;
    steps.push({ id: text(node, "id"), kind: text(node, "kind"), name: text(node, "name"), attributes: node.attributes && typeof node.attributes === "object" && !Array.isArray(node.attributes) ? node.attributes : {} });
  }
  const edges: Edge[] = [];
  for (const edge of Array.isArray(v.edges) ? v.edges : []) {
    if (!edge || typeof edge !== "object") return null;
    edges.push({ from: text(edge, "from"), to: text(edge, "to"), kind: text(edge, "kind") });
  }
  return { graph_id: text(v, "graph_id"), name: text(v, "name"), project: text(v, "project"), approval: text(v, "approval"), approved_at: text(v, "approved_at"), steps, edges };
}

/** The steps in dependency order; ties keep the graph's order; empty when there is a cycle. */
export function topologicalOrder(graph: Graph): string[] {
  const incoming = new Map<string, number>();
  for (const s of graph.steps) incoming.set(s.id, 0);
  for (const e of graph.edges) if (incoming.has(e.to) && incoming.has(e.from)) incoming.set(e.to, incoming.get(e.to)! + 1);
  const order: string[] = [];
  const placed = new Set<string>();
  while (order.length < graph.steps.length) {
    let progressed = false;
    for (const step of graph.steps) {
      if (placed.has(step.id) || incoming.get(step.id) !== 0) continue;
      order.push(step.id);
      placed.add(step.id);
      for (const e of graph.edges) if (e.from === step.id && incoming.has(e.to)) incoming.set(e.to, incoming.get(e.to)! - 1);
      progressed = true;
      break;
    }
    if (!progressed) return [];
  }
  return order;
}

export const LAYOUT = { columnWidth: 200, boxWidth: 164, boxHeight: 56, rowHeight: 84, top: 36, left: 18 };

/** Where render_svg places each step: one column per stage kind present (unknown kinds last, as
 * "other"), steps stacked in dependency order. */
export function layoutGraph(graph: Graph) {
  const known = (kind: string) => (STEP_KINDS as readonly string[]).includes(kind);
  const columns: string[] = STEP_KINDS.filter((kind) => graph.steps.some((s) => s.kind === kind));
  if (graph.steps.some((s) => !known(s.kind))) columns.push("other");
  let order = topologicalOrder(graph);
  if (!order.length) order = graph.steps.map((s) => s.id);
  const position = new Map<string, { x: number; y: number }>();
  const filled = new Map<string, number>();
  for (const id of order) {
    const step = graph.steps.find((s) => s.id === id)!;
    const column = known(step.kind) ? step.kind : "other";
    const index = columns.indexOf(column);
    const row = filled.get(column) ?? 0;
    filled.set(column, row + 1);
    position.set(id, { x: LAYOUT.left + index * LAYOUT.columnWidth, y: LAYOUT.top + row * LAYOUT.rowHeight });
  }
  const rows = Math.max(1, ...filled.values());
  return {
    columns,
    position,
    width: Math.max(1, columns.length) * LAYOUT.columnWidth + LAYOUT.left,
    height: LAYOUT.top + rows * LAYOUT.rowHeight,
  };
}

// --- A workspace's own solutions beside Orama's (lake#597, docs/api/solutions.md) -------------

/**
 * Who a solution is from: Orama's catalogue (read-only) or the workspace's own (editable). An
 * entry without `source` (an engine read) is Orama's.
 */
export type SolutionSource = "orama"|"workspace";
export function solutionSource(entry: J): SolutionSource {
    return text(entry, "source") === "workspace" ? "workspace" : "orama";
}

/** The badge a solution card and page carry: "Orama" or "Yours". */
export function sourceLabel(entry: J): "Orama"|"Yours" {
    return solutionSource(entry) === "workspace" ? "Yours" : "Orama";
}

/**
 * The solutions a toolbar keeps: by source, by Orama's run state (which only Orama's carry) and
 * by a search over the id, name and the text a person reads.
 */
export function filterSolutions(
    entries: J[],
    filter: {source: SolutionSource|null; state : string | null; search : string}): J[] {
    const needle = filter.search.trim().toLowerCase();
    return entries.filter((entry) => {
        if (filter.source && solutionSource(entry) !== filter.source)
            return false;
        if (filter.state &&
            (solutionSource(entry) !== "orama" || runsBadge(entry).label !== filter.state))
            return false;
        if (!needle)
            return true;
        return [ "id", "name", "promise", "problem", "use_case", "decision", "notes" ].some(
            (field) => text(entry, field).toLowerCase().includes(needle));
    });
}

/**
 * A solution's methodologies as a person types them: "M-1, M-3:2" (an id, and its minimum level
 * after a colon when above 1).
 */
export function formatMethodologies(list: unknown): string {
    return solutionMethodologies({methodologies : Array.isArray(list) ? list : []})
        .map((m) => (m.level > 1 ? `${m.id}:${m.level}` : m.id))
        .join(", ");
}

/**
 * Parses what `formatMethodologies` writes, refusing what lake would: a level outside 1-4, an id
 * with spaces, or an id listed twice.
 */
export function parseMethodologies(value: string): {id: string; minimum_level : number}[] {
    const out: {id: string; minimum_level : number}[] = [];
    for (const raw of value.split(/[,\n]/)) {
        const item = raw.trim();
        if (!item)
            continue;
        const [id, level, ...rest] = item.split(":").map((s) => s.trim());
        if (rest.length || !/^[A-Za-z0-9._-]{1,64}$/.test(id))
            throw new Error(`"${item}" is not a methodology id such as M-1 or M-3:2.`);
        const minimum = level === undefined || level === "" ? 1 : Number(level);
        if (!Number.isInteger(minimum) || minimum < 1 || minimum > 4)
            throw new Error(`${id}'s minimum level must be 1 to 4.`);
        if (out.some((m) => m.id === id))
            throw new Error(`${id} is listed twice.`);
        out.push({id, minimum_level : minimum});
    }
    return out;
}

/** The fields of a solution's form, as typed. */
export type SolutionForm = {
    name: string; problem : string; decision : string; metric : string; baseline : string;
    target : string;
    methodologies : string;
    pipeline : string;
    notes : string
};

export function solutionForm(solution: J|null): SolutionForm {
    const m: J = solution?.measurement && typeof solution.measurement === "object"
                     ? solution.measurement
                     : {};
    return {
        name : text(solution, "name"),
        problem : text(solution, "problem"),
        decision : text(solution, "decision"),
        metric : text(m, "metric"),
        baseline : text(m, "baseline"),
        target : text(m, "target"),
        methodologies : formatMethodologies(solution?.methodologies),
        pipeline : text(solution, "pipeline"),
        notes : text(solution, "notes"),
    };
}

/**
 * The input create_solution and update_solution take from the form (every field sent, so an
 * emptied one is cleared).
 */
export function solutionInput(form: SolutionForm): J {
    return {
        name : form.name.trim(),
        problem : form.problem.trim(),
        decision : form.decision.trim(),
        measurement : {
            metric : form.metric.trim(),
            baseline : form.baseline.trim(),
            target : form.target.trim()
        },
        methodologies : parseMethodologies(form.methodologies),
        pipeline : form.pipeline.trim(),
        notes : form.notes.trim(),
    };
}

/** A pipeline reference `<project>/<pipeline>` as a link's parts; null for a template's text. */
export function pipelineReference(value: string): {project: string; graph : string}|null {
    const match = /^([a-z0-9][a-z0-9_-]{0,62})\/([^/\s][^/]*)$/.exec(value.trim());
    return match ? {project : match[1], graph : match[2].trim()} : null;
}

/**
 * What a project started from one of your own solutions records as its decision: the decision it
 * drives and how its return is measured.
 */
export function startDecision(solution: J): string {
    const m: J = solution?.measurement && typeof solution.measurement === "object"
                     ? solution.measurement
                     : {};
    const metric = text(m, "metric");
    const from = text(m, "baseline");
    const to = text(m, "target");
    let measured = metric ? `Measured by ${metric}` : "";
    if (measured && from && to)
        measured += `, from ${from} to ${to}`;
    else if (measured && to)
        measured += `, to ${to}`;
    else if (measured && from)
        measured += `, from ${from}`;
    const decision = text(solution, "decision");
    return [ decision, measured ? `${measured}.` : "" ].filter(Boolean).join(" ");
}

/**
 * The sentence a project started from one of your own solutions builds its first pipeline from
 * (compile_pipeline's intent): the decision, for the problem.
 */
export function startIntent(solution: J): string {
    const decision = text(solution, "decision");
    const problem = text(solution, "problem");
    return [ decision, problem ? `The problem: ${problem}` : "" ].filter(Boolean).join(" ");
}

/** From a dataset to a model (lake#818). A dataset's page offers **Train a model on this**, which
 * opens Design → Projects with New project open for that dataset; creating the project opens its
 * page with Build pipeline open and the dataset chosen under From data. These are the addresses
 * and the suggested name; the pages add the workspace prefix. */
const query = (values: Record<string, string>) => new URLSearchParams(values).toString();

/** The dataset as From data names it: `<database>.<name>` (`datasetChoices`' `path`). */
export function datasetSource(database: string, name: string): string {
  return `${database || "datasets"}.${name}`;
}

/** Where a dataset's Train a model on this goes: Projects, New project open for the dataset. */
export function trainFromDatasetPath(database: string, name: string): string {
  return `/design/projects?${query({ new: "1", dataset: datasetSource(database, name) })}`;
}

/** Where creating a project for a dataset goes: the project, Build pipeline open on the dataset. */
export function buildFromDatasetPath(project: string, dataset: string): string {
  return `/design/project?${query(dataset ? { name: project, build: "1", dataset } : { name: project })}`;
}

/** A project name to start from for a dataset: a project's name rule (lowercase letters, digits,
 * - and _, starting with a letter or digit, at most 63), `<dataset>-model`. */
export function projectNameFor(dataset: string): string {
  const name = dataset.slice(dataset.lastIndexOf(".") + 1);
  const stem = name.toLowerCase().replace(/[^a-z0-9_-]+/g, "-").replace(/^[^a-z0-9]+/, "").replace(/-+$/, "");
  const suffix = "-model";
  return stem ? `${stem.slice(0, 63 - suffix.length).replace(/-+$/, "")}${suffix}` : "";
}

