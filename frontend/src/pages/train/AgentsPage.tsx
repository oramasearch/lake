/** Train → Agents (web_ui.cpp agents_content, web_ui_your_agents, agent_create_form): the space's
 * own agents, Create an Agent, and the Agent Library. Your Agents was a deferred panel because
 * Zetta's list_agents is slow (lake#446); here it is an ordinary read that fills in after the page
 * renders. */
import { useState } from "react";
import { Link, useNavigate } from "react-router-dom";
import type { Row } from "@/lib/api";
import { act } from "@/lib/api";
import { useSession } from "@/lib/session";
import { useToast } from "@/lib/toast";
import { useTrail } from "@/lib/trail";
import { Button, DataTable, ErrorBlock, Field, fieldError } from "@/lib/ui";
import { AGENT_SKILLS, AgentLibrary, engagement, modelLabel, skillLabels } from "./agents";
import { WorkspaceFrame, qs, text, useAsync, useWorkspace, useWq, useWsPath, wa } from "./workspace";

export default function AgentsPage() {
  const ws = useWsPath();
  const { customerId } = useWorkspace();
  const query = qs({ customer_id: customerId });
  useTrail([
    { label: "Train", to: ws(`/train/agents?${query}`) },
    { label: "Agents", to: ws(`/train/agents?${query}`) },
  ]);
  return (
    <WorkspaceFrame
      eyebrow="Train"
      title="Agents"
      sub="Build agents that work for you, run them, and see what they did. Talk to any of them in Ask Orama."
      always={<AgentLibrary />}
    >
      {() => (
        <>
          <YourAgents />
          <CreateAgent />
        </>
      )}
    </WorkspaceFrame>
  );
}

function YourAgents() {
  const ws = useWsPath();
  const { customerId } = useWorkspace();
  const listed = useWq<{ agents?: Row[] }>("list_agents");
  const agents = (listed.data?.agents ?? []).filter((a) => text(a, "kind") !== "orama");
  return (
    <section className="panel" id="your-agents" aria-busy={listed.isLoading || undefined}>
      <h2>Your Agents</h2>
      {listed.isLoading ? (
        <p className="muted" role="status">
          Loading your agents from the workspace…
        </p>
      ) : (
        <DataTable
          q={listed}
          what="your agents"
          rows={agents}
          empty={<p>No agents yet. Create one below.</p>}
          rowKey={(a) => text(a, "id")}
          columns={[
            {
              key: "agent",
              label: "Agent",
              render: (a) => (
                <span className="agent-cell">
                  <Link to={ws(`/train/agent?${qs({ customer_id: customerId, id: text(a, "id") })}`)}>{text(a, "name")}</Link>
                  <small>{text(a, "purpose")}</small>
                </span>
              ),
            },
            { key: "engagement", label: "Engagement", render: (a) => <span className="badge">{engagement(a)}</span> },
            { key: "skills", label: "Skills", render: (a) => skillLabels(a.skills) },
            { key: "model", label: "Model", render: (a) => <code>{modelLabel(a)}</code> },
            { key: "status", label: "Status", render: (a) => <span className="badge">{text(a, "status") || "idle"}</span> },
          ]}
        />
      )}
    </section>
  );
}

type Draft = {
  name: string;
  purpose: string;
  work_area: string;
  work_area_detail: string;
  manifesto: string;
  goal: string;
  skills: string[];
  model: "base" | "frontier";
  trigger: string;
};

const BLANK: Draft = { name: "", purpose: "", work_area: "workspace", work_area_detail: "", manifesto: "", goal: "", skills: [], model: "base", trigger: "" };

/** Create an Agent (the `agent_create` form handler): the Orama base model makes a native agent that
 * runs in the workspace; a frontier model makes a Python agent that runs on agent-runtime from the
 * instructions composed here, and its trigger is scheduled. */
function CreateAgent() {
  const ws = useWsPath();
  const navigate = useNavigate();
  const toast = useToast();
  const { viewer } = useSession();
  const scope = useWorkspace();
  const [draft, setDraft] = useState<Draft>(BLANK);
  const create = useAsync();
  const set = <K extends keyof Draft>(key: K, value: Draft[K]) => setDraft((d) => ({ ...d, [key]: value }));
  const query = qs({ customer_id: scope.customerId });

  const submit = () =>
    create.run(async () => {
      const { name, purpose, manifesto, goal, trigger } = draft;
      const agent: Row = {
        name,
        purpose,
        description: purpose,
        work_area: { kind: draft.work_area, detail: draft.work_area_detail },
        manifesto,
        goal,
        skills: draft.skills,
        trigger,
      };
      if (draft.model === "frontier") {
        let instructions = `# ${name}\n\n${purpose}\n`;
        if (manifesto) instructions += `\n## How you work\n\n${manifesto}\n`;
        if (goal) instructions += `\n## Your goal\n\n${goal}\n`;
        agent.runtime = "python";
        agent.model_name = viewer.config?.frontier_model ?? "gpt-4o";
        agent.definition = { source: "zetta", instructions, files: [], entrypoint: "" };
      } else {
        agent.runtime = "native";
        agent.model = viewer.config?.base_agent_model ?? "";
        agent.model_name = "Orama base model";
        agent.system_prompt = manifesto;
      }
      const created = await wa<{ id?: string }>(scope, "create_agent", agent);
      const id = created.id ?? "";
      if (agent.runtime === "python" && trigger) {
        // A trigger is lake's to fire (docs/api/agent-launches.md).
        try {
          await act("schedule_agent", { customer_id: scope.customerId, workspace_id: scope.workspaceId, agent_id: id, trigger });
        } catch (error) {
          toast((error as Error).message, "bad");
        }
      }
      setDraft(BLANK);
      navigate(id ? ws(`/train/agent?${qs({ customer_id: scope.customerId, id })}`) : ws(`/train/agents?${query}`));
    });

  return (
    <section className="panel" id="create-agent">
      <h2>Create an Agent</h2>
      <p>
        <Button to={ws(`/train/agents/new?${query}`)}>Build it with the Agent Factory</Button> or fill in the form below.
      </p>
      <p className="sub">
        An agent does one job for you. Say what the job is, how it should work, what success looks like and what it may use. Without a schedule it
        answers only when you talk to it; with one it also runs on its own.
      </p>
      <form
        className="stack"
        onSubmit={(e) => {
          e.preventDefault();
          void submit();
        }}
      >
        {create.error && !fieldError(create.error, "name") && !fieldError(create.error, "purpose") ? <ErrorBlock error={create.error} /> : null}
        <Field label="Name" error={fieldError(create.error, "name")}>
          <input name="name" required maxLength={80} placeholder="Renewal watcher" value={draft.name} onChange={(e) => set("name", e.target.value)} />
        </Field>
        <Field label="What it's for" error={fieldError(create.error, "purpose")}>
          <input name="purpose" required placeholder="Flag renewals at risk a month ahead" value={draft.purpose} onChange={(e) => set("purpose", e.target.value)} />
        </Field>
        <Field label="Where it works">
          <select name="work_area" value={draft.work_area} onChange={(e) => set("work_area", e.target.value)}>
            <option value="workspace">Anywhere in this workspace</option>
            <option value="project">One project</option>
            <option value="dataset">Specific datasets</option>
          </select>
        </Field>
        <Field label="Which project or datasets (if not the whole workspace)">
          <input name="work_area_detail" placeholder="churn" value={draft.work_area_detail} onChange={(e) => set("work_area_detail", e.target.value)} />
        </Field>
        <Field label="Instructions" help="How it should work, and what it must never do.">
          <textarea
            name="manifesto"
            rows={3}
            placeholder="Answer from company data only. Say when evidence is missing rather than estimating."
            value={draft.manifesto}
            onChange={(e) => set("manifesto", e.target.value)}
          />
        </Field>
        <Field label="What success looks like">
          <textarea name="goal" rows={2} placeholder="Every at-risk renewal is flagged a month ahead." value={draft.goal} onChange={(e) => set("goal", e.target.value)} />
        </Field>
        <fieldset>
          <legend>What it may do</legend>
          {AGENT_SKILLS.map(([key, label]) => (
            <label key={key}>
              <input
                type="checkbox"
                name="skills"
                value={key}
                checked={draft.skills.includes(key)}
                onChange={(e) => set("skills", e.target.checked ? [...draft.skills, key] : draft.skills.filter((s) => s !== key))}
              />{" "}
              {label}
            </label>
          ))}
        </fieldset>
        <Field label="Model">
          <select name="model" value={draft.model} onChange={(e) => set("model", e.target.value as Draft["model"])}>
            <option value="base">Orama base model, inside your workspace</option>
            <option value="frontier">A frontier model (OpenAI), run by lake</option>
          </select>
        </Field>
        <Field label="Runs on its own" help="Leave empty to use it only in conversation.">
          <input name="trigger" placeholder="every Monday at 09:00" value={draft.trigger} onChange={(e) => set("trigger", e.target.value)} />
        </Field>
        <div>
          <Button variant="primary" type="submit" loading={create.busy}>
            Create agent
          </Button>
        </div>
      </form>
    </section>
  );
}
