/** Train → Agents (web_ui.cpp agents_content, web_ui_your_agents, agent_create_form): the space's
 * own agents as cards with figures and a toolbar, New agent in a drawer, the Agent Factory beside it,
 * and the Agent Library. Your Agents was a deferred panel because Zetta's list_agents is slow
 * (lake#446); here it is an ordinary read that fills in after the page renders. */
import { useState } from "react";
import { useNavigate } from "react-router-dom";
import { Bot, Clock, Cpu, MessageSquare, Plus, Sparkles } from "lucide-react";
import type { Row } from "@/lib/api";
import { act } from "@/lib/api";
import { Drawer } from "@/lib/overlay";
import { useSession } from "@/lib/session";
import { useToast } from "@/lib/toast";
import { useTrail } from "@/lib/trail";
import { Button, Card, Chip, Empty, ErrorBlock, Field, FilterChips, SearchInput, Skeleton, Stat, StatRow, Status, Toolbar, fieldError } from "@/lib/ui";
import { AGENT_SKILLS, AgentLibrary, engagement, modelLabel, runsWhere, skillLabels } from "./agents";
import { WorkspaceFrame, qs, text, useAsync, useWorkspace, useWorkspaceReady, useWq, useWsPath, wa } from "./workspace";

export default function AgentsPage() {
  const ws = useWsPath();
  const { customerId } = useWorkspace();
  const ready = useWorkspaceReady();
  const query = qs({ customer_id: customerId });
  const [creating, setCreating] = useState(false);
  useTrail([
    { label: "Train", to: ws(`/train/agents?${query}`) },
    { label: "Agents", to: ws(`/train/agents?${query}`) },
  ]);
  return (
    <>
      <WorkspaceFrame
        eyebrow="Train"
        title="Agents"
        sub="Build agents that work for you, run them, and see what they did. Talk to any of them in Ask Orama."
        actions={
          ready ? (
            <>
              <Button to={ws(`/train/agents/new?${query}`)}>
                <Sparkles size={14} aria-hidden /> Build with the Agent Factory
              </Button>
              <Button variant="primary" onClick={() => setCreating(true)}>
                <Plus size={14} aria-hidden /> New agent
              </Button>
            </>
          ) : null
        }
        always={<AgentLibrary />}
      >
        {() => <YourAgents onNew={() => setCreating(true)} />}
      </WorkspaceFrame>
      {creating && <CreateAgent onClose={() => setCreating(false)} />}
    </>
  );
}

const CONVERSATION = "Conversation";
const UNATTENDED = "Conversation and unattended";

function YourAgents({ onNew }: { onNew: () => void }) {
  const ws = useWsPath();
  const { customerId } = useWorkspace();
  const listed = useWq<{ agents?: Row[] }>("list_agents");
  const [search, setSearch] = useState("");
  const [mode, setMode] = useState<string | null>(null);
  if (listed.isLoading)
    return (
      <div aria-busy="true">
        <p className="muted" role="status">
          Loading your agents from the workspace…
        </p>
        <Skeleton />
      </div>
    );
  if (listed.isError) return <ErrorBlock error={listed.error} what="your agents" retry={() => void listed.refetch()} />;
  const agents = (listed.data?.agents ?? []).filter((a) => text(a, "kind") !== "orama");
  const unattended = agents.filter((a) => engagement(a) === UNATTENDED).length;
  const frontier = agents.filter((a) => text(a, "runtime") === "python").length;
  const needle = search.trim().toLowerCase();
  const shown = agents.filter(
    (a) => (!mode || engagement(a) === mode) && (!needle || `${text(a, "name")} ${text(a, "purpose")} ${modelLabel(a)}`.toLowerCase().includes(needle)),
  );
  return (
    <section id="your-agents" aria-label="Your agents">
      <StatRow>
        <Stat label="Agents" icon={Bot} value={agents.length} sub="in this workspace" />
        <Stat label="Run on their own" icon={Clock} value={unattended} sub="have a trigger" />
        <Stat label="Conversation only" icon={MessageSquare} value={agents.length - unattended} sub="answer when you talk to them" />
        <Stat label="Frontier model" icon={Cpu} value={frontier} sub="run by lake on agent-runtime" />
      </StatRow>
      {agents.length === 0 ? (
        <Empty
          icon={Bot}
          title="No agents yet"
          action={
            <>
              <Button variant="primary" onClick={onNew}>
                <Plus size={14} aria-hidden /> New agent
              </Button>
              <Button to={ws(`/train/agents/new?${qs({ customer_id: customerId })}`)}>Build with the Agent Factory</Button>
            </>
          }
        >
          An agent does one job for you. Describe it yourself, or let AgentCreator ask what it needs and propose one.
        </Empty>
      ) : (
        <>
          <Toolbar>
            <SearchInput value={search} onChange={setSearch} placeholder="Search agents…" />
            <FilterChips
              value={mode}
              onChange={setMode}
              options={[
                { id: CONVERSATION, label: "Conversation only", count: agents.length - unattended },
                { id: UNATTENDED, label: "Runs on its own", count: unattended },
              ]}
            />
          </Toolbar>
          {shown.length === 0 ? (
            <Empty compact title="No agent matches">
              Clear the search or pick another filter.
            </Empty>
          ) : (
            <div className="grid grid-cols-1 gap-3 md:grid-cols-2 xl:grid-cols-3">
              {shown.map((agent) => (
                <AgentCard key={text(agent, "id")} agent={agent} to={ws(`/train/agent?${qs({ customer_id: customerId, id: text(agent, "id") })}`)} />
              ))}
            </div>
          )}
        </>
      )}
    </section>
  );
}

function AgentCard({ agent, to }: { agent: Row; to: string }) {
  const trigger = text(agent, "trigger");
  return (
    <Card to={to} label={runsWhere(agent)} title={text(agent, "name")} actions={<Status value={text(agent, "status") || "idle"} />} className="h-full">
      <p className="m-0 line-clamp-2 text-[13px] text-ink-2">{text(agent, "purpose") || "—"}</p>
      <div className="mt-3 flex flex-wrap items-center gap-1.5">
        {trigger ? (
          <Chip tone="action" title="Runs on its own">
            <Clock size={12} aria-hidden /> {trigger}
          </Chip>
        ) : (
          <Chip title="Answers when you talk to it">
            <MessageSquare size={12} aria-hidden /> Conversation
          </Chip>
        )}
        {modelLabel(agent) && (
          <Chip tone="ai" title="Model">
            <Cpu size={12} aria-hidden /> {modelLabel(agent)}
          </Chip>
        )}
      </div>
      {skillLabels(agent.skills) !== "—" && <p className="mt-2 mb-0 text-[12px] text-ink-3">May: {skillLabels(agent.skills)}</p>}
    </Card>
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

/** New agent (the `agent_create` form handler): the Orama base model makes a native agent that runs
 * in the workspace; a frontier model makes a Python agent that runs on agent-runtime from the
 * instructions composed here, and its trigger is scheduled. */
function CreateAgent({ onClose }: { onClose: () => void }) {
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
      onClose();
      navigate(id ? ws(`/train/agent?${qs({ customer_id: scope.customerId, id })}`) : ws(`/train/agents?${query}`));
    });

  return (
    <Drawer
      title="New Agent"
      subtitle="Say what the job is, how it should work, what success looks like and what it may use."
      onClose={onClose}
      width={580}
      footer={
        <>
          <Button to={ws(`/train/agents/new?${query}`)} className="mr-auto">
            <Sparkles size={14} aria-hidden /> Use the Agent Factory instead
          </Button>
          <Button onClick={onClose}>Cancel</Button>
          <Button variant="primary" type="submit" form="create-agent" loading={create.busy}>
            Create agent
          </Button>
        </>
      }
    >
      <form
        id="create-agent"
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
        <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
          <Field label="Where it works">
            <select name="work_area" value={draft.work_area} onChange={(e) => set("work_area", e.target.value)}>
              <option value="workspace">Anywhere in this workspace</option>
              <option value="project">One project</option>
              <option value="dataset">Specific datasets</option>
            </select>
          </Field>
          <Field label="Which project or datasets" help="If not the whole workspace.">
            <input name="work_area_detail" placeholder="churn" value={draft.work_area_detail} onChange={(e) => set("work_area_detail", e.target.value)} />
          </Field>
        </div>
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
        <Field label="What it may do">
          <div className="flex flex-wrap gap-1.5" role="group" aria-label="What it may do">
            {AGENT_SKILLS.map(([key, label]) => {
              const on = draft.skills.includes(key);
              return (
                <Chip key={key} active={on} onClick={() => set("skills", on ? draft.skills.filter((s) => s !== key) : [...draft.skills, key])}>
                  {label}
                </Chip>
              );
            })}
          </div>
        </Field>
        <Field label="Model">
          <select name="model" value={draft.model} onChange={(e) => set("model", e.target.value as Draft["model"])}>
            <option value="base">Orama base model, inside your workspace</option>
            <option value="frontier">A frontier model (OpenAI), run by lake</option>
          </select>
        </Field>
        <Field label="Runs on its own" help="Leave empty to use it only in conversation.">
          <input name="trigger" placeholder="every Monday at 09:00" value={draft.trigger} onChange={(e) => set("trigger", e.target.value)} />
        </Field>
      </form>
    </Drawer>
  );
}
