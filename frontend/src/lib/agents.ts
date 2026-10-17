/** What Design → Agents and Train → Agents decide about an agent (lake#653). Pure, so `npm test`
 * runs it (tests/agents.test.ts).
 *
 * - Design → Agents lists Orama's agents (the Agent Library, read-only, "By Orama") beside the
 *   workspace's own (editable, "Yours"), like Design → Solutions (lake#597).
 * - Train → Agents lists only the workspace's agents with something to train: those whose
 *   behaviour comes from a model, a pipeline or a training schedule. */

import agentSkills from "../pages/train/agent-skills.json" with { type: "json" };

// eslint-disable-next-line @typescript-eslint/no-explicit-any
export type J = Record<string, any>;

export function text(row: J | null | undefined, key: string): string {
  const value = row?.[key];
  if (value === null || value === undefined) return "";
  if (typeof value === "string") return value;
  if (typeof value === "number" || typeof value === "boolean") return String(value);
  return "";
}

/* ------------------------------------------------------------------ who an agent is from */

/** Orama's (the Agent Library, or an engine record of kind `orama`) or the workspace's own. */
export type AgentSource = "orama" | "workspace";

/** A library entry carries a `key` (its repository) and no engine id; an engine record of kind
 * `orama` is one Orama put in the workspace. Everything else is the workspace's own. */
export function agentSource(agent: J): AgentSource {
  if (text(agent, "kind") === "orama") return "orama";
  if (text(agent, "key") && !text(agent, "id")) return "orama";
  return "workspace";
}

export function sourceLabel(agent: J): string {
  return agentSource(agent) === "orama" ? "By Orama" : "Yours";
}

/** Whether a person may change an agent's definition here: only the workspace's own. Orama's are
 * read-only and offer Duplicate instead. */
export function editable(agent: J): boolean {
  return agentSource(agent) === "workspace";
}

/** The workspace's own agents in a `list_agents` answer: the records Orama put there are Orama's. */
export function ownAgents(agents: unknown): J[] {
  return (Array.isArray(agents) ? agents : []).filter((a): a is J => !!a && typeof a === "object" && agentSource(a) === "workspace");
}

/** What a library agent's badge says, and whether a person may talk to it (lake#763): whether this
 * Lake runs it is always shown. Every library agent is an ordinary agent (lake#788): an `internal`
 * flag from an older Lake changes nothing. */
export function libraryState(agent: J): { label: string; running: boolean; talkable: boolean } {
  const running = text(agent, "status") === "Available" || agent.deployed === true;
  const label = running ? "Available" : text(agent, "runs_as") === "offline" ? "Offline tool" : "Not running on this Lake";
  return { label, running, talkable: running && agent.chat !== false };
}

/** The Design → Agents filter: who it is from, and a search over its name and purpose. */
export function filterAgents(agents: J[], { source, search }: { source: AgentSource | null; search: string }): J[] {
  const needle = search.trim().toLowerCase();
  return agents.filter((agent) => {
    if (source && agentSource(agent) !== source) return false;
    if (!needle) return true;
    return [text(agent, "name"), text(agent, "purpose"), text(agent, "description"), text(agent, "used_in"), text(agent, "key"), modelLabel(agent)]
      .join(" ")
      .toLowerCase()
      .includes(needle);
  });
}

/* ------------------------------------------------------------------ what it runs on */

export function modelLabel(agent: J): string {
  return text(agent, "model_name") || text(agent, "model");
}

/** Where an agent runs, in words. */
export function runsWhere(agent: J): string {
  const runtime = text(agent, "runtime");
  return runtime === "python" ? "On agent-runtime" : runtime === "service" ? "As its own service" : "In your workspace";
}

/** A native agent (the engine runs it on a model it can train): no runtime, or `native`. */
export function isNative(agent: J): boolean {
  const runtime = text(agent, "runtime");
  return !runtime || runtime === "native";
}

export function pipelinesOf(agent: J): { project: string; graph: string; version: string }[] {
  return (Array.isArray(agent.pipelines) ? agent.pipelines : [])
    .filter((g: unknown): g is J => !!g && typeof g === "object")
    .map((g: J) => ({ project: text(g, "project"), graph: text(g, "graph"), version: text(g, "version") }));
}

/** A training schedule that retrains on its own (anything but `manual`). */
export function hasTrainingSchedule(agent: J): boolean {
  const training = agent.training && typeof agent.training === "object" ? (agent.training as J) : null;
  const cadence = text(training, "cadence");
  return !!cadence && cadence !== "manual";
}

/** Whether Train → Agents lists an agent: its behaviour comes from a model the engine trains (a
 * native agent with a model), a pipeline it calls, or a training schedule. A frontier-model agent
 * with no pipeline has nothing Lake can train. Orama's are not listed. */
export function trainable(agent: J): boolean {
  if (agentSource(agent) !== "workspace") return false;
  if (isNative(agent) && !!text(agent, "model")) return true;
  if (pipelinesOf(agent).length > 0) return true;
  return hasTrainingSchedule(agent);
}

/** Why an agent is on Train → Agents, in words. */
export function trainableBecause(agent: J): string[] {
  const why: string[] = [];
  if (isNative(agent) && text(agent, "model")) why.push("model");
  if (pipelinesOf(agent).length) why.push(pipelinesOf(agent).length === 1 ? "1 pipeline" : `${pipelinesOf(agent).length} pipelines`);
  if (hasTrainingSchedule(agent)) why.push(`retrains ${text(agent.training, "cadence").replace("_", " ")}`);
  return why;
}

/* ------------------------------------------------------------------ skills */

/** The skills a person may grant an agent, with their labels, in the runtime's order: agent-runtime's
 * own skills.json, vendored by tools/agents/vendor-agent-skills.py (agent-runtime#7), the one list
 * the pages, the engine's `create_agent` schema and a launch's scopes all follow. Never written
 * here. */
export const AGENT_SKILLS: [string, string][] = (agentSkills as unknown as [string, string][]).filter((s) => Array.isArray(s) && typeof s[0] === "string");

/** The keys of the skills the runtime offers. */
export const OFFERED_SKILLS: string[] = AGENT_SKILLS.map(([key]) => key);

/** A skill's label: the runtime's, or, for one it no longer lists, its own name made readable. */
export function skillLabel(skill: string): string {
  const known = AGENT_SKILLS.find(([key]) => key === skill);
  if (known) return known[1];
  const words = skill.replace(/^mcp:/, "MCP: ").replace(/[-_]+/g, " ").trim();
  return words.charAt(0).toUpperCase() + words.slice(1);
}

export function skillLabels(skills: unknown): string {
  const labels = (Array.isArray(skills) ? skills : []).filter((s): s is string => typeof s === "string").map(skillLabel);
  return labels.length ? labels.join(", ") : "—";
}

/** The skills offered in a form: the runtime's, then any the agent already has that it no longer
 * lists (kept, so saving does not drop them silently). */
export function skillChoices(offered: string[], current: unknown): string[] {
  const out = [...offered];
  for (const s of Array.isArray(current) ? current : []) if (typeof s === "string" && !out.includes(s)) out.push(s);
  return out;
}

/* ------------------------------------------------------------------ the definition form */

export type ModelChoice = "base" | "frontier" | "workspace";

export type AgentForm = {
  name: string;
  purpose: string;
  instructions: string;
  goal: string;
  skills: string[];
  model: ModelChoice;
  /** A workspace model (`model:<id>`) when `model` is "workspace". */
  workspaceModel: string;
  workArea: "workspace" | "project" | "dataset";
  workAreaDetail: string;
  pipelines: { project: string; graph: string }[];
  /** Runs on its own: a schedule in words or cron; empty is conversation only. */
  trigger: string;
  /** The workspace streams whose new messages each start a run (lake#677): Lake's
   * `set_agent_streams`, not a field of the engine's record, so Edit reads them with
   * `list_agent_streams`. */
  streams: string[];
};

export const BLANK_AGENT: AgentForm = {
  name: "",
  purpose: "",
  instructions: "",
  goal: "",
  skills: [],
  model: "base",
  workspaceModel: "",
  workArea: "workspace",
  workAreaDetail: "",
  pipelines: [],
  trigger: "",
  streams: [],
};

/** The form for an agent record (Edit, Duplicate), or blank. */
export function agentForm(agent: J | null): AgentForm {
  if (!agent) return { ...BLANK_AGENT, skills: [], pipelines: [], streams: [] };
  const area = agent.work_area && typeof agent.work_area === "object" ? (agent.work_area as J) : {};
  const kind = text(area, "kind");
  const runtime = text(agent, "runtime");
  const model = text(agent, "model");
  const choice: ModelChoice = runtime === "python" ? "frontier" : model.startsWith("model:") ? "workspace" : "base";
  return {
    name: text(agent, "name"),
    purpose: text(agent, "purpose") || text(agent, "description"),
    instructions: text(agent, "manifesto") || text(agent, "system_prompt") || text(agent.definition, "instructions"),
    goal: text(agent, "goal"),
    skills: (Array.isArray(agent.skills) ? agent.skills : []).filter((s: unknown): s is string => typeof s === "string"),
    model: choice,
    workspaceModel: choice === "workspace" ? model : "",
    workArea: kind === "project" || kind === "dataset" ? kind : "workspace",
    workAreaDetail: text(area, "detail"),
    pipelines: pipelinesOf(agent).map(({ project, graph }) => ({ project, graph })),
    trigger: text(agent, "trigger"),
    streams: [],
  };
}

/* ------------------------------------------------------------------ streams that start it */

/** System streams Lake and its agents write (Train → Streams marks them): never a trigger, so an
 * agent's own events cannot start it again. Lake refuses them too. */
export function isSystemStream(name: string): boolean {
  return ["agent-", "app-events.", "build-events.", "run-metrics.", "study-events.", "inbox:"].some((p) => name.startsWith(p));
}

/** The streams an agent may start from: the workspace Zetta's own topics in a `list_streams`
 * answer (its `groups`), by name, without the system streams or the Lake bus. */
export function triggerStreams(groups: unknown): string[] {
  const out: string[] = [];
  for (const group of Array.isArray(groups) ? (groups as J[]) : []) {
    if (!group || typeof group !== "object" || text(group, "zetta") !== "workspace") continue;
    for (const topic of Array.isArray(group.topics) ? (group.topics as J[]) : []) {
      const name = text(topic, "topic").replace(/^topic:/, "");
      if (name && topic.system !== true && !isSystemStream(name) && !out.includes(name)) out.push(name);
    }
  }
  return out.sort((a, b) => a.localeCompare(b));
}

/** What `set_agent_streams` should be sent after saving the form, or null when nothing changes: only
 * a frontier-model agent runs from a stream, so one that becomes another kind loses its streams. */
export function streamsToSet(form: AgentForm, before: string[] = []): string[] | null {
  const after = form.model === "frontier" ? [...new Set(form.streams)] : [];
  const same = after.length === before.length && after.every((s) => before.includes(s));
  return same ? null : after;
}

/** A frontier agent's instructions, composed from what the person wrote. */
export function composedInstructions(form: AgentForm): string {
  let out = `# ${form.name.trim()}\n\n${form.purpose.trim()}\n`;
  if (form.instructions.trim()) out += `\n## How you work\n\n${form.instructions.trim()}\n`;
  if (form.goal.trim()) out += `\n## Your goal\n\n${form.goal.trim()}\n`;
  return out;
}

/** What create_agent or update_agent takes for a form: the Studio shape (purpose, work area,
 * manifesto, goal, skills, trigger), the approved pipelines it may call, and the model, which
 * decides where it runs. The Orama base model makes a native agent in the workspace (Lake fills in
 * its checkpoint); a workspace model is one trained here (`model:<id>`); a frontier model makes a
 * Python agent run on agent-runtime from composed instructions. */
export function agentInput(form: AgentForm, options: { frontierModel?: string; existing?: J | null } = {}): J {
  const name = form.name.trim();
  const purpose = form.purpose.trim();
  const input: J = {
    name,
    purpose,
    description: purpose,
    work_area: { kind: form.workArea, detail: form.workArea === "workspace" ? "" : form.workAreaDetail.trim() },
    manifesto: form.instructions.trim(),
    goal: form.goal.trim(),
    skills: [...form.skills],
    pipelines: form.pipelines.map(({ project, graph }) => ({ project, graph })),
    trigger: form.trigger.trim(),
    kind: "custom",
  };
  if (form.model === "frontier") {
    input.runtime = "python";
    input.model_name = options.frontierModel || text(options.existing, "model_name") || "gpt-4o";
    const existing = options.existing?.definition && typeof options.existing.definition === "object" ? (options.existing.definition as J) : null;
    // An edit keeps the definition's own files and entrypoint; only the instructions are composed.
    input.definition =
      existing && text(existing, "source") === "lake"
        ? existing
        : { source: "zetta", instructions: composedInstructions(form), files: Array.isArray(existing?.files) ? existing!.files : [], entrypoint: text(existing, "entrypoint") };
  } else {
    input.runtime = "native";
    input.system_prompt = form.instructions.trim();
    if (form.model === "workspace" && form.workspaceModel) {
      input.model = form.workspaceModel;
      input.model_name = form.workspaceModel;
    } else {
      // An empty model: Lake gives a native agent its base model (lake#594).
      input.model = options.existing && isNative(options.existing) && !text(options.existing, "model").startsWith("model:") ? text(options.existing, "model") : "";
      input.model_name = "Orama base model";
    }
  }
  return input;
}

/** What the form refuses before anything is sent: a name and a purpose, and a model for "a model
 * trained here". The field is the input's own (`name`, `purpose`, `model`). */
export function formProblem(form: AgentForm): { field: string; message: string } | null {
  if (!form.name.trim()) return { field: "name", message: "An agent needs a name." };
  if (!form.purpose.trim()) return { field: "purpose", message: "Say what the agent is for." };
  if (form.model === "workspace" && !form.workspaceModel) return { field: "model", message: "Choose one of this workspace's models." };
  return null;
}

/** The name a copy gets: "<name> (copy)", or "(copy 2)" and on when that is taken. */
export function copyName(name: string, taken: string[] = []): string {
  const base = name.replace(/ \(copy(?: \d+)?\)$/, "");
  let candidate = `${base} (copy)`;
  for (let n = 2; taken.includes(candidate); n++) candidate = `${base} (copy ${n})`;
  return candidate;
}

/** Duplicate one of Orama's library agents: the form, prefilled with what the library says of it,
 * for the person to finish and create as their own. The library holds no prompt, so the
 * instructions start from its purpose. */
export function duplicateLibraryForm(agent: J, taken: string[] = []): AgentForm {
  const purpose = text(agent, "purpose");
  return {
    ...BLANK_AGENT,
    skills: [],
    pipelines: [],
    name: copyName(text(agent, "name"), taken),
    purpose,
    instructions: purpose ? `Do what ${text(agent, "name")} does: ${purpose}` : "",
  };
}

/** Duplicate one of the workspace's own agents: create_agent with its definition and a new name. */
export function duplicateInput(agent: J, taken: string[] = []): J {
  const input = definitionOf(agent);
  input.name = copyName(text(agent, "name"), taken);
  input.kind = "custom";
  delete input.is_default;
  return input;
}

/** The fields of a record a person designs (as Lake keeps each revision): not its id, status,
 * training, trained versions or timestamps. */
const DESIGNED = [
  "name",
  "description",
  "purpose",
  "kind",
  "runtime",
  "work_area",
  "manifesto",
  "goal",
  "goals",
  "skills",
  "trigger",
  "model",
  "model_name",
  "model_version",
  "system_prompt",
  "tools",
  "pipelines",
  "definition",
  "endpoint",
  "chat_capable",
  "is_default",
];

export function definitionOf(agent: J): J {
  const out: J = {};
  for (const key of DESIGNED) if (agent[key] !== undefined && agent[key] !== null) out[key] = structuredClone(agent[key]);
  // A pipeline grant is re-approved on write; its pinned version is the engine's to set.
  if (Array.isArray(out.pipelines)) out.pipelines = pipelinesOf(out).map(({ project, graph }) => ({ project, graph }));
  return out;
}

/** What changed between two revisions' definitions, as field names in words. */
export function revisionChanges(before: J | null | undefined, after: J): string[] {
  if (!before) return ["Created"];
  const words: Record<string, string> = {
    name: "name",
    purpose: "purpose",
    description: "purpose",
    manifesto: "instructions",
    system_prompt: "instructions",
    goal: "goal",
    goals: "goals",
    skills: "skills",
    trigger: "schedule",
    model: "model",
    model_name: "model",
    model_version: "model",
    runtime: "model",
    pipelines: "pipelines",
    work_area: "work area",
    definition: "instructions",
    tools: "tools",
    endpoint: "endpoint",
  };
  const changed = new Set<string>();
  for (const key of new Set([...Object.keys(before), ...Object.keys(after)])) {
    if (JSON.stringify(before[key] ?? null) !== JSON.stringify(after[key] ?? null)) changed.add(words[key] ?? key);
  }
  return [...changed];
}

/* ------------------------------------------------------------------ training */

export type TrainedVersion = { checkpoint: string; created_at: string; job: string; corpus: string; fingerprint: string; eval_score: number | null };

/** The versions `agent_training_status` answers, oldest first, a score below 0 meaning not
 * evaluated. */
export function trainedVersions(status: J | null | undefined): TrainedVersion[] {
  const list = Array.isArray(status?.versions) ? (status!.versions as J[]) : [];
  return list
    .filter((v) => !!v && typeof v === "object")
    .map((v) => ({
      checkpoint: text(v, "checkpoint"),
      created_at: text(v, "created_at"),
      job: text(v, "job"),
      corpus: text(v, "corpus"),
      fingerprint: text(v, "fingerprint"),
      eval_score: typeof v.eval_score === "number" && v.eval_score >= 0 ? v.eval_score : null,
    }))
    .sort((a, b) => a.created_at.localeCompare(b.created_at));
}

/** The version the agent runs now: the one whose checkpoint is its model, or null. */
export function currentVersion(agent: J, versions: TrainedVersion[]): TrainedVersion | null {
  const model = text(agent, "model");
  return versions.find((v) => v.checkpoint && v.checkpoint === model) ?? null;
}

/** The best evaluated version (highest held-out score), or null. */
export function bestVersion(versions: TrainedVersion[]): TrainedVersion | null {
  let best: TrainedVersion | null = null;
  for (const v of versions) if (v.eval_score !== null && (best === null || v.eval_score > (best.eval_score ?? -Infinity))) best = v;
  return best;
}

/** What switching to a version is: Promote when it is newer than the current one (or there is
 * none), Roll back when it is older; null for the current one. */
export function switchKind(version: TrainedVersion, current: TrainedVersion | null): "promote" | "rollback" | null {
  if (current && version.checkpoint === current.checkpoint) return null;
  if (!current) return "promote";
  return version.created_at > current.created_at ? "promote" : "rollback";
}

/** update_agent's input to run a version: its checkpoint becomes the agent's model. */
export function switchInput(agentId: string, version: TrainedVersion): J {
  return { id: agentId, model: version.checkpoint, model_version: version.created_at };
}

/** The held-out score over time as chart columns: [times in seconds, scores]. */
export function scoreSeries(versions: TrainedVersion[]): [number[], (number | null)[]] {
  const xs: number[] = [];
  const ys: (number | null)[] = [];
  for (const v of versions) {
    const at = Date.parse(v.created_at);
    if (Number.isNaN(at) || v.eval_score === null) continue;
    xs.push(at / 1000);
    ys.push(v.eval_score);
  }
  return [xs, ys];
}

/** How an agent's unattended runs went: how many finished, the share that succeeded, and the
 * median time from start to finish when the runs say when they finished. */
export function runFigures(launches: J[]): { finished: number; succeeded: number; successRate: number | null; medianSeconds: number | null } {
  let succeeded = 0;
  let failed = 0;
  const durations: number[] = [];
  for (const launch of launches) {
    const phase = text(launch, "phase");
    if (phase === "Succeeded") succeeded++;
    else if (phase === "Failed") failed++;
    else continue;
    const start = Date.parse(text(launch, "started_at") || text(launch, "created_at"));
    const end = Date.parse(text(launch, "finished_at") || text(launch, "completed_at"));
    if (!Number.isNaN(start) && !Number.isNaN(end) && end >= start) durations.push((end - start) / 1000);
  }
  const finished = succeeded + failed;
  durations.sort((a, b) => a - b);
  const medianSeconds = durations.length ? durations[Math.floor((durations.length - 1) / 2)] : null;
  return { finished, succeeded, successRate: finished ? succeeded / finished : null, medianSeconds };
}

export const CADENCES: { value: string; label: string }[] = [
  { value: "manual", label: "Only when you start it" },
  { value: "on_change", label: "When its tools or goals change" },
  { value: "daily", label: "Daily" },
  { value: "weekly", label: "Weekly" },
  { value: "monthly", label: "Monthly" },
];

/** set_training_schedule's `training` from the form: empty fields are left to the engine. */
export function trainingInput(form: { cadence: string; corpus: string; base_checkpoint: string; steps: string }): J {
  const training: J = { cadence: form.cadence || "manual" };
  if (form.corpus.trim()) training.corpus = form.corpus.trim();
  if (form.base_checkpoint.trim()) training.base_checkpoint = form.base_checkpoint.trim();
  const steps = Number(form.steps);
  if (form.steps.trim() && Number.isInteger(steps) && steps >= 0) training.steps = steps;
  return training;
}

/** What an experiment compares, as Experiments Agent is asked it. */
export type ExperimentAxis = "versions" | "prompts" | "models";

/** The ask the Experiments Agent turns into an editable experiment proposal for an agent: which agent,
 * what it compares, what it should improve, and what to measure. */
export function experimentAsk(agent: J, axis: ExperimentAxis, goal: string, versions: TrainedVersion[]): string {
  const name = text(agent, "name") || text(agent, "id");
  const what =
    axis === "versions"
      ? `its trained versions (${versions.map((v) => v.checkpoint).filter(Boolean).join(", ") || "none yet"})`
      : axis === "prompts"
        ? "variations of its instructions"
        : "the models it could run on";
  let ask = `Design an experiment for the agent "${name}" (id ${text(agent, "id")}) that compares ${what}.`;
  if (goal.trim()) ask += ` What it should improve: ${goal.trim()}.`;
  else if (text(agent, "goal")) ask += ` Its goal: ${text(agent, "goal")}.`;
  ask += " Measure the held-out score, the task success rate, latency and cost per run, and propose which to promote.";
  return ask;
}

/* ------------------------------------------------------------------ Ask Orama's Talk to */

/** One entry of Ask Orama's *Talk to* (control::talk_to_agents, lake#763): the name and purpose come
 * from the Agent Library, never written here; `running` false is listed, marked and not selectable. */
export type TalkToAgent = { id: string; name: string; purpose: string; usedIn: string; running: boolean };

export const ASK_ORAMA_KEY = "agent-ask-orama";

/** Who Ask Orama can talk to: Ask Orama first, then every other library chat agent (one this Lake
 * does not run is listed with `running: false` rather than left out), then the space's own agents
 * that take a conversation. No library agent is left out (lake#788). The agent in the URL stays
 * listed while the lists load. */
export function talkToAgents(library: J[] | undefined, own: J[] | undefined, selected: string): TalkToAgent[] {
  const entry = (agent: J | undefined, key: string): TalkToAgent => ({
    id: key,
    // Until the library answers, a library agent is not named at all rather than named twice.
    name: text(agent, "name") || (library ? key : "Loading…"),
    purpose: text(agent, "purpose"),
    usedIn: text(agent, "used_in"),
    running: !agent || agent.deployed === true || key === ASK_ORAMA_KEY,
  });
  const rows = library ?? [];
  const agents: TalkToAgent[] = [entry(rows.find((a) => text(a, "key") === ASK_ORAMA_KEY), ASK_ORAMA_KEY)];
  const keys = new Set<string>();
  for (const agent of rows) {
    const key = text(agent, "key");
    keys.add(key);
    if (key && key !== ASK_ORAMA_KEY && agent.chat === true) agents.push(entry(agent, key));
  }
  for (const agent of own ?? []) {
    const id = text(agent, "id");
    if (!id || (text(agent, "kind") || "custom") === "orama" || agent.chat_capable === false || keys.has(id)) continue;
    const name = text(agent, "name") || id;
    const purpose = text(agent, "purpose") || text(agent, "description");
    const known = agents.find((a) => a.id === id);
    if (known) Object.assign(known, { name, purpose });
    else agents.push({ id, name, purpose, usedIn: "", running: true });
  }
  if (selected && !agents.some((a) => a.id === selected)) agents.push({ id: selected, name: selected, purpose: "", usedIn: "", running: true });
  return agents;
}
