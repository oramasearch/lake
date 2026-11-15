// SPDX-License-Identifier: MIT

/** Train → Agents (lake#653): training and improving the agents that learn. It lists only the
 * workspace's agents with something to train, those whose behaviour comes from a model, a
 * pipeline or a training schedule, each with how its training stands (`agent_training_status`).
 * Designing an agent is Design → Agents; each card opens the agent's training page. */
import { useQueries } from "@tanstack/react-query";
import { useState } from "react";
import { AlertTriangle, Bot, CalendarClock, GraduationCap, PenLine } from "lucide-react";
import { read, type Row } from "@/lib/api";
import { useTrail } from "@/lib/trail";
import { Button, Card, Chip, Empty, ErrorBlock, SearchInput, Skeleton, Stat, StatRow, Status, Toolbar } from "@/lib/ui";
import { bestVersion, currentVersion, filterAgents, modelLabel, ownAgents, trainable, trainableBecause, trainedVersions, text } from "@/lib/agents";
import { useEngineRead } from "./agents";
import { WorkspaceFrame, qs, useWorkspace, useWsPath } from "./workspace";

export default function AgentsPage() {
  const ws = useWsPath();
  useTrail([
    { label: "Train", to: ws("/train/agents") },
    { label: "Agents", to: ws("/train/agents") },
  ]);
  return (
    <WorkspaceFrame
      eyebrow="Train"
      title="Agents"
      sub="Train and improve the agents that learn: those that run on a model, call a pipeline or retrain on a schedule. See how each performs, launch experiments, and promote a better version."
      actions={
        <Button to={ws("/design/agents")}>
          <PenLine size={14} aria-hidden /> Design an agent
        </Button>
      }
    >
      {() => <TrainableAgents />}
    </WorkspaceFrame>
  );
}

type TrainingStatus = { trained?: boolean; stale?: boolean; due?: boolean; reason?: string; versions?: Row[] };

function TrainableAgents() {
  const ws = useWsPath();
  const scope = useWorkspace();
  const listed = useEngineRead<{ agents?: Row[] }>("list_agents");
  const [search, setSearch] = useState("");
  const agents = ownAgents(listed.data?.agents).filter(trainable);
  // One training status per trainable agent, read beside the list.
  const statuses = useQueries({
    queries: agents.map((agent) => ({
      queryKey: ["agent_training_status", scope.customerId, scope.workspaceId, text(agent, "id")],
      queryFn: ({ signal }: { signal: AbortSignal }) => read<TrainingStatus>("agent_training_status", { customer_id: scope.customerId, workspace_id: scope.workspaceId, id: text(agent, "id") }, { signal }),
      retry: false,
      staleTime: 30_000,
    })),
  });
  if (listed.isLoading)
    return (
      <div aria-busy="true">
        <p className="muted" role="status">
          Loading the agents from the workspace…
        </p>
        <Skeleton />
      </div>
    );
  if (listed.isError) return <ErrorBlock error={listed.error} what="the agents" retry={() => void listed.refetch()} />;
  const all = ownAgents(listed.data?.agents);
  const statusOf = (id: string) => statuses[agents.findIndex((a) => text(a, "id") === id)]?.data;
  const stale = agents.filter((a) => statusOf(text(a, "id"))?.stale).length;
  const due = agents.filter((a) => statusOf(text(a, "id"))?.due).length;
  const versions = agents.reduce((sum, a) => sum + trainedVersions(statusOf(text(a, "id"))).length, 0);
  const shown = filterAgents(agents, { source: null, search });
  if (!agents.length)
    return (
      <Empty icon={GraduationCap} title="No Agent to Train Yet" action={<Button to={ws("/design/agents")}>Design an agent</Button>}>
        {all.length
          ? `This workspace's ${all.length === 1 ? "agent runs" : `${all.length} agents run`} on a frontier model and call no pipeline, so there is nothing to train. An agent on the Orama base model, or one granted a pipeline, appears here.`
          : "An agent that runs on the Orama base model, calls a pipeline or retrains on a schedule appears here. Design one first."}
      </Empty>
    );
  return (
    <section aria-label="Agents to train">
      <StatRow>
        <Stat label="Agents to train" icon={Bot} value={agents.length} sub={`of ${all.length} in this workspace`} />
        <Stat label="Trained versions" icon={GraduationCap} value={versions} sub="saved by training" />
        <Stat label="Stale" icon={AlertTriangle} value={stale} tone={stale ? "bad" : undefined} sub="tools or goals changed since" />
        <Stat label="Due" icon={CalendarClock} value={due} sub="their schedule says retrain" />
      </StatRow>
      <Toolbar>
        <SearchInput value={search} onChange={setSearch} placeholder="Search agents…" label="Search agents" />
      </Toolbar>
      {shown.length === 0 ? (
        <Empty compact title="No agent matches">
          Clear the search.
        </Empty>
      ) : (
        <div className="grid grid-cols-1 gap-3 md:grid-cols-2 xl:grid-cols-3">
          {shown.map((agent) => {
            const id = text(agent, "id");
            const status = statusOf(id);
            const list = trainedVersions(status);
            const best = bestVersion(list);
            const current = currentVersion(agent, list);
            return (
              <Card
                key={id}
                to={ws(`/train/agent?${qs({ id })}`)}
                label={trainableBecause(agent).join(" · ")}
                title={text(agent, "name")}
                actions={status?.stale ? <Status value="Stale" tone="danger" /> : status?.due ? <Status value="Due" tone="warning" /> : <Status value={list.length ? "Trained" : "Untrained"} tone={list.length ? "success" : "neutral"} />}
                className="h-full"
              >
                <p className="m-0 line-clamp-2 text-[13px] text-ink-2">{text(agent, "purpose") || "—"}</p>
                <div className="mt-3 flex flex-wrap items-center gap-1.5">
                  {modelLabel(agent) && <Chip tone="ai">{modelLabel(agent)}</Chip>}
                  <Chip>{list.length === 1 ? "1 version" : `${list.length} versions`}</Chip>
                  {best?.eval_score !== null && best?.eval_score !== undefined && <Chip tone="ok">best held-out {best.eval_score.toFixed(3)}</Chip>}
                  {current && current !== best && <Chip tone="warn">a better version exists</Chip>}
                </div>
                {status?.reason && <p className="mt-2 mb-0 text-[12px] text-ink-3">{status.reason}</p>}
              </Card>
            );
          })}
        </div>
      )}
    </section>
  );
}
