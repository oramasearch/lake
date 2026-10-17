/** Design → Agents (lake#653, renamed from Agent Library): where agents are designed. It works as
 * Design → Solutions does (lake#597): Orama's agents (the Agent Library, read-only, "By Orama",
 * with Duplicate on their page) beside the workspace's own (editable, "Yours"), a filter by who an
 * agent is from and a search. All nine of Orama's are listed in one list (lake#763, lake#788),
 * each saying whether this Lake runs it. New
 * agent designs one in a drawer; the Agent Factory, where
 * Agent Creator Agent drafts one from a description, is the other way in. Running and training an agent
 * are on Train → Agent, linked from each agent. */
import { useState } from "react";
import { Bot, Clock, Cpu, MessageSquare, Plus, SearchX, Sparkles } from "lucide-react";
import type { Row } from "@/lib/api";
import { useRead } from "@/lib/api";
import { Badge, Button, Card, Chip, Empty, ErrorBlock, FilterChips, Grid, Page, SearchInput, Section, Skeleton, Stat, StatRow, Status, Toolbar } from "@/lib/ui";
import { WorkspaceGate, useWorkspaceStatus } from "@/components/WorkspaceGate";
import { agentForm, agentSource, filterAgents, libraryState, modelLabel, ownAgents, skillLabels, sourceLabel, text, type AgentForm, type AgentSource } from "@/lib/agents";
import { useEngineRead } from "@/pages/train/agents";
import { qs, useWsPath } from "./api";
import { useDesignTrail } from "./parts";
import { NewAgent } from "./AgentDrawers";

export default function AgentsPage() {
  useDesignTrail({ label: "Agents", path: "/design/agents" }, [], "Agents");
  const ws = useWsPath();
  const ready = useWorkspaceStatus().q.data?.ready === true;
  const [creating, setCreating] = useState<AgentForm | null>(null);
  return (
    <Page
      eyebrow="Design"
      title="Agents"
      sub="Design the agents that work for you: what each is for, its prompt, its skills, the models and pipelines it may use. Orama's agents are read-only; duplicate one to make it your own."
      actions={
        ready ? (
          <>
            <Button to={ws("/design/agents/new")}>
              <Sparkles size={14} aria-hidden /> Describe it to the Agent Factory
            </Button>
            <Button variant="primary" onClick={() => setCreating(agentForm(null))}>
              <Plus size={14} aria-hidden /> New agent
            </Button>
          </>
        ) : undefined
      }
    >
      <WorkspaceGate after={ready ? undefined : <OramaAgentsOnly />}>
        <Agents onNew={(form) => setCreating(form)} />
        {creating && <NewAgent initial={creating} onClose={() => setCreating(null)} />}
      </WorkspaceGate>
    </Page>
  );
}

/** Orama's agents only, under the gate while the workspace is not ready: the library is Lake's,
 * not the workspace's. */
function OramaAgentsOnly() {
  const library = useLibrary();
  return (
    <div className="mt-8">
      <Section title="By Orama" sub="Agents Orama provides, each for one job. Start your workspace to design your own or duplicate one of these.">
        {library.isLoading ? <Skeleton /> : library.isError ? <ErrorBlock error={library.error} what="Orama's agents" /> : <AgentCards agents={library.data?.agents ?? []} />}
      </Section>
    </div>
  );
}

/** Every agent in Orama's library, all of them ordinary agents (lake#763, lake#788). */
function useLibrary() {
  return useRead<{ agents?: Row[] }>("list_agent_library", {}, { staleTime: 300_000 });
}

function Agents({ onNew }: { onNew: (form: AgentForm) => void }) {
  const ws = useWsPath();
  const [search, setSearch] = useState("");
  const [source, setSource] = useState<AgentSource | null>(null);
  const library = useLibrary();
  const listed = useEngineRead<{ agents?: Row[] }>("list_agents");
  if (listed.isLoading || library.isLoading)
    return (
      <div aria-busy="true">
        <p className="muted" role="status">
          Loading the agents…
        </p>
        <Skeleton />
      </div>
    );
  if (listed.isError) return <ErrorBlock error={listed.error} what="your agents" retry={() => void listed.refetch()} />;
  const yours = ownAgents(listed.data?.agents);
  const orama: Row[] = library.isError ? [] : library.data?.agents ?? [];
  const shown = filterAgents([...yours, ...orama], { source, search });
  const shownYours = shown.filter((a) => agentSource(a) === "workspace");
  const shownOrama = shown.filter((a) => agentSource(a) === "orama");
  const unattended = yours.filter((a) => text(a, "trigger")).length;
  const filtering = !!search.trim() || !!source;
  return (
    <>
      <StatRow cols={3}>
        <Stat label="Yours" value={yours.length} icon={Bot} sub="designed in this workspace" />
        <Stat label="By Orama" value={orama.length} sub="read-only, duplicate to edit" />
        <Stat label="Run on their own" value={unattended} icon={Clock} sub="of yours, on a schedule" />
      </StatRow>
      <Toolbar>
        <SearchInput value={search} onChange={setSearch} placeholder="Search agents…" label="Search agents" />
        <FilterChips<AgentSource>
          allLabel="All"
          options={[
            { id: "orama", label: "By Orama", count: orama.length },
            { id: "workspace", label: "Yours", count: yours.length },
          ]}
          value={source}
          onChange={setSource}
        />
      </Toolbar>
      {filtering && !shown.length && (
        <Empty compact icon={SearchX} title="No Agent Matches">
          Clear the search or the filter to see more.
        </Empty>
      )}
      {source !== "orama" && (shownYours.length > 0 || !filtering) && (
        <Section title="Yours">
          {shownYours.length ? (
            <AgentCards agents={shownYours} />
          ) : (
            <Empty
              compact
              icon={Bot}
              title="No Agents of Your Own Yet"
              action={
                <>
                  <Button variant="primary" onClick={() => onNew(agentForm(null))}>
                    <Plus size={14} aria-hidden /> New agent
                  </Button>
                  <Button to={ws("/design/agents/new")}>Describe it to the Agent Factory</Button>
                </>
              }
            >
              An agent does one job for you. Design one yourself, describe it and let the Agent Creator Agent draft it, or open one of Orama's and duplicate it.
            </Empty>
          )}
        </Section>
      )}
      {source !== "workspace" && shownOrama.length > 0 && (
        <Section title="By Orama" sub="Read-only. Open one to see where you meet it, or duplicate it to make your own.">
          <AgentCards agents={shownOrama} />
        </Section>
      )}
      {source !== "workspace" && library.isError && <ErrorBlock error={library.error} what="Orama's agents" retry={() => void library.refetch()} />}
    </>
  );
}

function AgentCards({ agents }: { agents: Row[] }) {
  const ws = useWsPath();
  return (
    <Grid className="grid-cols-1 gap-3 md:grid-cols-2 xl:grid-cols-3">
      {agents.map((agent, index) => {
        const own = agentSource(agent) === "workspace";
        const to = own ? ws(`/design/agent?${qs({ id: text(agent, "id") })}`) : ws(`/design/agent?${qs({ key: text(agent, "key") || text(agent, "id") })}`);
        const state = libraryState(agent);
        return (
          <Card
            key={text(agent, "id") || text(agent, "key") || index}
            to={to}
            title={
              <span className="inline-flex items-center gap-2">
                <Bot size={15} className="text-ink-3" aria-hidden /> {text(agent, "name")}
              </span>
            }
            actions={
              <span className="inline-flex flex-wrap items-center gap-1">
                <Badge tone={own ? "success" : "info"} title={own ? "Your workspace's own: you can edit it" : "Orama's: read-only, duplicate it to edit"}>
                  {sourceLabel(agent)}
                </Badge>
                {own && <Status value={text(agent, "status") || "idle"} />}
              </span>
            }
            className="flex h-full flex-col"
          >
            <p className="m-0 line-clamp-3 text-[13px] text-ink-2">{text(agent, "purpose") || text(agent, "description") || "—"}</p>
            {own ? (
              <div className="mt-3 flex flex-wrap items-center gap-1.5">
                {text(agent, "trigger") ? (
                  <Chip tone="action" title="Runs on its own">
                    <Clock size={12} aria-hidden /> {text(agent, "trigger")}
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
            ) : (
              <>
                {text(agent, "used_in") && (
                  <p className="mt-2 mb-0 text-[12.5px] text-ink-3">
                    <span className="label mr-1 inline">Where</span>
                    {text(agent, "used_in")}
                  </p>
                )}
                {/* Whether this Lake runs it, for every one of Orama's agents (lake#763). */}
                <div className="mt-3 flex flex-wrap items-center gap-1.5">
                  <Status value={state.label} tone={state.running ? "success" : "neutral"} />
                </div>
              </>
            )}
            {own && skillLabels(agent.skills) !== "—" && <p className="mt-2 mb-0 text-[12px] text-ink-3">Skills: {skillLabels(agent.skills)}</p>}
          </Card>
        );
      })}
    </Grid>
  );
}
