/** What Design → Agents, Design → Agent, Train → Agents, Train → Agent and the Agent Factory share
 * (lake#653): the skills the runtime offers, the agent definition form, and the workspace reads
 * through Lake's named engine capabilities (`list_agents`, `get_agent`, …, lake#650). The pure
 * rules are in `@/lib/agents`. */
import { useQuery } from "@tanstack/react-query";
import { ApiError, useRead, type Row } from "@/lib/api";
import { Chip, Field, Form, FormError, fieldError } from "@/lib/ui";
import { skillChoices, skillLabel, skillsFromOpenApi, text, type AgentForm } from "@/lib/agents";
import { useWorkspace } from "./workspace";

export { modelLabel, runsWhere, skillLabel, skillLabels } from "@/lib/agents";

export function engagement(agent: Row): string {
  return text(agent, "trigger") ? "Conversation and unattended" : "Conversation";
}

/* ------------------------------------------------------------------ reads */

/** A named engine read in the page's workspace (`list_agents`, `get_agent`,
 * `agent_training_status`, `list_models`, `list_projects`): `GET /v1/<name>` with the space and
 * workspace, as REST and MCP callers call it. */
export function useEngineRead<T = Row>(
  name: string | null,
  input: Record<string, string | number | undefined> = {},
  options: { enabled?: boolean; retry?: boolean | number; refetchInterval?: number | false } = {},
) {
  const { customerId, workspaceId } = useWorkspace();
  return useRead<T>(name, { customer_id: customerId, workspace_id: workspaceId, ...input }, { ...options, enabled: !!customerId && !!workspaceId && (options.enabled ?? true) });
}

/** The OpenAPI document (the same query Developer → API reads). */
export function useOpenApi() {
  return useQuery<Row>({
    queryKey: ["openapi.json"],
    staleTime: 300_000,
    queryFn: async ({ signal }) => {
      let response: Response;
      try {
        response = await fetch("/v1/openapi.json", { headers: { Accept: "application/json" }, credentials: "same-origin", signal });
      } catch (error) {
        if ((error as Error).name === "AbortError") throw error;
        throw new ApiError(0, "network", "Lake could not be reached. Check your connection and retry.");
      }
      if (!response.ok) throw new ApiError(response.status, "error", `The OpenAPI document did not load (${response.status}).`);
      return (await response.json()) as Row;
    },
  });
}

/** The skills an agent may be given: whatever the runtime offers, read from `create_agent`'s
 * schema in Lake's OpenAPI (never a list written here). */
export function useAgentSkills(): { skills: string[]; loading: boolean; error: unknown } {
  const doc = useOpenApi();
  return { skills: skillsFromOpenApi(doc.data), loading: doc.isLoading, error: doc.error };
}

/** Approved pipelines a grant may take, without the Project Agent's check graphs. */
export function approvedPipelines(projects: unknown): { project: string; graph: string }[] {
  const out: { project: string; graph: string }[] = [];
  for (const project of Array.isArray(projects) ? (projects as Row[]) : []) {
    if (!project || typeof project !== "object") continue;
    for (const graph of Array.isArray(project.graphs) ? (project.graphs as Row[]) : []) {
      const name = text(graph, "name");
      if (name.startsWith("Project Agent check") || text(graph, "approval") !== "approved") continue;
      out.push({ project: text(project, "name"), graph: name });
    }
  }
  return out;
}

/* ------------------------------------------------------------------ the definition form */

/** An agent's definition as a person designs it: purpose and prompt, skills, the model and the
 * pipelines it may use, its work area, and when it runs on its own. Shared by New agent, Edit and
 * Duplicate. */
export function AgentFields({ id, form, onChange, onSubmit, error }: { id: string; form: AgentForm; onChange: (form: AgentForm) => void; onSubmit: () => void | Promise<unknown>; error: unknown }) {
  const offered = useAgentSkills();
  const models = useEngineRead<{ models?: Row[] }>("list_models", {}, { retry: false });
  const projects = useEngineRead<{ projects?: Row[] }>("list_projects", {}, { retry: false });
  const set = <K extends keyof AgentForm>(key: K, value: AgentForm[K]) => onChange({ ...form, [key]: value });
  const skills = skillChoices(offered.skills, form.skills);
  const workspaceModels = (models.data?.models ?? []).filter((m) => !!m && typeof m === "object");
  const modelValue = (m: Row) => `model:${text(m, "id") || text(m, "model_id")}`;
  const approved = approvedPipelines(projects.data?.projects);
  for (const grant of form.pipelines) if (!approved.some((a) => a.project === grant.project && a.graph === grant.graph)) approved.push(grant);
  const granted = (p: { project: string; graph: string }) => form.pipelines.some((g) => g.project === p.project && g.graph === p.graph);
  const inField = ["name", "purpose", "model", "skills", "pipelines", "trigger"].some((f) => fieldError(error, f));
  return (
    <Form id={id} onSubmit={onSubmit}>
      <FormError error={inField ? null : error} />
      <Field label="Name" required error={fieldError(error, "name")}>
        <input name="name" maxLength={80} placeholder="Renewal watcher" value={form.name} onChange={(e) => set("name", e.target.value)} />
      </Field>
      <Field label="What it's for" required error={fieldError(error, "purpose")}>
        <input name="purpose" placeholder="Flag renewals at risk a month ahead" value={form.purpose} onChange={(e) => set("purpose", e.target.value)} />
      </Field>
      <Field label="Instructions" help="Its prompt: how it should work, and what it must never do.">
        <textarea
          name="instructions"
          rows={4}
          placeholder="Answer from company data only. Say when evidence is missing rather than estimating."
          value={form.instructions}
          onChange={(e) => set("instructions", e.target.value)}
        />
      </Field>
      <Field label="What success looks like">
        <textarea name="goal" rows={2} placeholder="Every at-risk renewal is flagged a month ahead." value={form.goal} onChange={(e) => set("goal", e.target.value)} />
      </Field>
      <Field
        label="Skills"
        help={offered.error ? "The skills could not be read; the agent keeps the ones it has." : "What it may do. Each skill is granted, never assumed."}
        error={fieldError(error, "skills")}
      >
        <div className="flex flex-wrap gap-1.5" role="group" aria-label="Skills">
          {offered.loading && !skills.length ? <span className="muted text-[12.5px]">Reading the skills the runtime offers…</span> : null}
          {skills.map((key) => {
            const on = form.skills.includes(key);
            return (
              <Chip key={key} active={on} onClick={() => set("skills", on ? form.skills.filter((s) => s !== key) : [...form.skills, key])}>
                {skillLabel(key)}
              </Chip>
            );
          })}
        </div>
      </Field>
      <Field label="Model" error={fieldError(error, "model")}>
        <select
          name="model"
          value={form.model === "workspace" ? form.workspaceModel || "workspace" : form.model}
          onChange={(e) => {
            const value = e.target.value;
            if (value === "base" || value === "frontier") onChange({ ...form, model: value, workspaceModel: "" });
            else onChange({ ...form, model: "workspace", workspaceModel: value === "workspace" ? "" : value });
          }}
        >
          <option value="base">Orama base model, inside your workspace (trainable)</option>
          <option value="frontier">A frontier model (OpenAI), run by Lake</option>
          {workspaceModels.length ? (
            <option value="workspace" disabled>
              A model trained in this workspace:
            </option>
          ) : null}
          {workspaceModels.map((m) => (
            <option key={modelValue(m)} value={modelValue(m)}>
              {text(m, "name") || modelValue(m)}
            </option>
          ))}
          {form.model === "workspace" && form.workspaceModel && !workspaceModels.some((m) => modelValue(m) === form.workspaceModel) ? <option value={form.workspaceModel}>{form.workspaceModel}</option> : null}
        </select>
      </Field>
      <Field label="Pipelines it may use" help="An approved pipeline granted here becomes one of its tools; the grant keeps the approved version." error={fieldError(error, "pipelines")}>
        {approved.length ? (
          <div className="flex flex-wrap gap-1.5" role="group" aria-label="Pipelines it may use">
            {approved.map((p) => (
              <Chip
                key={`${p.project}/${p.graph}`}
                active={granted(p)}
                onClick={() => set("pipelines", granted(p) ? form.pipelines.filter((g) => !(g.project === p.project && g.graph === p.graph)) : [...form.pipelines, p])}
              >
                {p.graph} <span className="text-ink-3">in {p.project}</span>
              </Chip>
            ))}
          </div>
        ) : (
          <span className="muted text-[12.5px]">{projects.isLoading ? "Reading the approved pipelines…" : "No approved pipeline yet. Approve one in a project to grant it here."}</span>
        )}
      </Field>
      <div className="grid grid-cols-1 gap-x-3 sm:grid-cols-2">
        <Field label="Where it works">
          <select name="work_area" value={form.workArea} onChange={(e) => set("workArea", e.target.value as AgentForm["workArea"])}>
            <option value="workspace">Anywhere in this workspace</option>
            <option value="project">One project</option>
            <option value="dataset">Specific datasets</option>
          </select>
        </Field>
        <Field label="Which project or datasets" help="If not the whole workspace.">
          <input name="work_area_detail" placeholder="churn" disabled={form.workArea === "workspace"} value={form.workAreaDetail} onChange={(e) => set("workAreaDetail", e.target.value)} />
        </Field>
      </div>
      <Field label="Runs on its own" help="A schedule, in words or five cron fields. Leave empty to use it only in conversation." error={fieldError(error, "trigger")}>
        <input name="trigger" placeholder="every Monday at 09:00" value={form.trigger} onChange={(e) => set("trigger", e.target.value)} />
      </Field>
    </Form>
  );
}
