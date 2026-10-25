/** Design → Agent (lake#653): one agent's design. One of the workspace's own (`?id=`, read with
 * `get_agent`) shows its definition, where it is used and every revision of it
 * (`list_agent_revisions`), with Edit (a new revision), Restore, Duplicate and Delete. One of
 * Orama's (`?key=`, from the Agent Library, or an engine record of kind `orama`) is read-only and
 * offers Duplicate to edit. Where it is used lists the workspace streams that start it (lake#677).
 * Running it (once, scheduled or launched) and training it are on Train → Agent, linked from here. */
import { useState, type ReactNode } from "react";
import { Link, useNavigate, useSearchParams } from "react-router-dom";
import { Bot, Copy, GraduationCap, History, MessageSquare, Pencil, Play, Radio, Trash2, Workflow } from "lucide-react";
import { ApiError, act, useRead, type Row } from "@/lib/api";
import { ConfirmButton } from "@/lib/overlay";
import { useToast } from "@/lib/toast";
import { Badge, Button, Chip, DataTable, Empty, ErrorBlock, KeyValue, Mono, Page, Skeleton, Split, Status } from "@/lib/ui";
import { WorkspaceGate, useWorkspaceStatus } from "@/components/WorkspaceGate";
import {
  agentSource,
  duplicateInput,
  duplicateLibraryForm,
  libraryState,
  modelLabel,
  pipelinesOf,
  revisionChanges,
  runsWhere,
  skillLabel,
  text,
  trainable,
  type AgentForm,
} from "@/lib/agents";
import { useEngineRead } from "@/pages/train/agents";
import { qs, useScope, useScopedRead, useWsPath } from "./api";
import { Panel, When, entityCrumb, useDesignTrail } from "./parts";
import { EditAgent, NewAgent } from "./AgentDrawers";
import { deleteAgent, restoreRevision } from "./agentActions";

export default function AgentPage() {
  const [params] = useSearchParams();
  const id = params.get("id") ?? "";
  const key = params.get("key") ?? "";
  return key && !id ? <OramaAgentPage agentKey={key} /> : <OwnAgentPage id={id} />;
}

/* ------------------------------------------------------------------ one of Orama's */

function OramaAgentPage({ agentKey }: { agentKey: string }) {
  const ready = useWorkspaceStatus().q.data?.ready === true;
  const library = useRead<{ agents?: Row[] }>("list_agent_library", {}, { staleTime: 300_000 });
  const agent = (library.data?.agents ?? []).find((a) => text(a, "key") === agentKey) ?? null;
  const name = agent ? text(agent, "name") : agentKey;
  useDesignTrail({ label: "Agents", path: "/design/agents" }, [entityCrumb(name)], `Agent: ${name}`);
  const [duplicating, setDuplicating] = useState<AgentForm | null>(null);
  const state = agent ? libraryState(agent) : null;
  return (
    <Page
      eyebrow="Design · Orama's Agent"
      title={
        agent ? (
          <span className="inline-flex flex-wrap items-center gap-2">
            {name}
            <Badge tone="info" title="Orama's: read-only, duplicate it to edit">
              By Orama
            </Badge>
            {state && <Status value={state.label} tone={state.running ? "success" : "neutral"} />}
          </span>
        ) : (
          agentKey
        )
      }
      sub={agent ? text(agent, "purpose") : undefined}
      actions={
        agent ? (
          <>
            {state?.talkable && (
              <Button to={`/ask?${qs({ agent: agentKey })}`}>
                <MessageSquare size={14} aria-hidden /> Talk in Ask Orama
              </Button>
            )}
            {ready && (
              <Button variant="primary" onClick={() => setDuplicating(duplicateLibraryForm(agent))}>
                <Copy size={14} aria-hidden /> Duplicate to edit
              </Button>
            )}
          </>
        ) : undefined
      }
    >
      {library.isLoading ? (
        <Skeleton />
      ) : library.isError ? (
        <ErrorBlock error={library.error} what="Orama's agents" retry={() => void library.refetch()} />
      ) : !agent ? (
        <MissingAgent>Orama provides no agent by that key.</MissingAgent>
      ) : (
        <Split
          side={
            <>
              <Panel label="Source" title={<Badge tone="info">By Orama</Badge>}>
                <KeyValue
                  rows={[
                    ["From", "Orama's Agent Library, read-only"],
                    ["Key", <Mono key="k">{agentKey}</Mono>],
                    ["Runs as", text(agent, "runs_as") === "agent-runtime" ? "On agent-runtime" : text(agent, "runs_as") === "service" ? "Its own service" : text(agent, "runs_as")],
                  ]}
                />
              </Panel>
              <Panel title="Make It Your Own">
                <p className="mt-0 text-[13px] text-ink-2">
                  Orama's agents are read-only. Duplicate this one to design your own from it: its purpose and prompt, skills, model and pipelines, in your workspace.
                </p>
                {ready ? (
                  <Button variant="primary" onClick={() => setDuplicating(duplicateLibraryForm(agent))}>
                    <Copy size={14} aria-hidden /> Duplicate to edit
                  </Button>
                ) : (
                  <p className="muted m-0 text-[12.5px]">Start your workspace to duplicate it.</p>
                )}
              </Panel>
            </>
          }
        >
          <Panel title="What It Does">
            <KeyValue
              rows={[
                ["What it does for you", text(agent, "purpose")],
                ["Where you meet it", text(agent, "used_in")],
                ["Talk to it in Ask Orama", state?.talkable ? "Yes" : agent.chat === true ? "Not while it is not running on this Lake" : "No"],
                ["On this Lake", state?.label],
              ]}
            />
          </Panel>
        </Split>
      )}
      {duplicating && <NewAgent title="Duplicate to Edit" initial={duplicating} onClose={() => setDuplicating(null)} />}
    </Page>
  );
}

function MissingAgent({ children }: { children: ReactNode }) {
  const ws = useWsPath();
  return (
    <Empty icon={Bot} title="No Such Agent" action={<Button to={ws("/design/agents")}>All agents</Button>}>
      {children}
    </Empty>
  );
}

/* ------------------------------------------------------------------ one of the workspace's */

type Open = "edit" | null;

function OwnAgentPage({ id }: { id: string }) {
  const ready = useWorkspaceStatus().q.data?.ready === true;
  const got = useEngineRead<Row>(id ? "get_agent" : null, { id }, { retry: false, enabled: ready });
  const agent = got.data && text(got.data, "id") ? got.data : null;
  const own = !!agent && agentSource(agent) === "workspace";
  const name = agent ? text(agent, "name") : "";
  useDesignTrail({ label: "Agents", path: "/design/agents" }, [id ? entityCrumb(name || id) : { label: "Agent" }], id ? `Agent: ${name || id}` : "Agent");
  const [open, setOpen] = useState<Open>(null);
  return (
    <Page
      eyebrow={own ? "Design · Your Agent" : "Design · Agent"}
      title={
        agent ? (
          <span className="inline-flex flex-wrap items-center gap-2">
            {name}
            <Badge tone={own ? "success" : "info"} title={own ? "Your workspace's own: you can edit it" : "Orama's: read-only, duplicate it to edit"}>
              {own ? "Yours" : "By Orama"}
            </Badge>
            <Status value={text(agent, "status") || "idle"} />
          </span>
        ) : (
          id || "Agent"
        )
      }
      sub={agent ? text(agent, "purpose") || text(agent, "description") || undefined : undefined}
      actions={agent ? <HeaderActions agent={agent} own={own} onEdit={() => setOpen("edit")} /> : undefined}
    >
      <WorkspaceGate>
        {!id ? (
          <MissingAgent>Open an agent from Design → Agents.</MissingAgent>
        ) : got.isLoading ? (
          <Skeleton />
        ) : got.isError ? (
          got.error instanceof ApiError && (got.error.status === 404 || got.error.code === "not_found") ? (
            <MissingAgent>It may have been deleted, or the link names another workspace.</MissingAgent>
          ) : (
            <ErrorBlock error={got.error} retry={() => void got.refetch()} what="the agent" />
          )
        ) : !agent ? (
          <MissingAgent>It may have been deleted, or the link names another workspace.</MissingAgent>
        ) : (
          <AgentDesign agent={agent} own={own} />
        )}
        {open === "edit" && agent && own && <EditAgent agent={agent} onClose={() => setOpen(null)} />}
      </WorkspaceGate>
    </Page>
  );
}

/** Yours: Edit, Duplicate, Delete, and Run it. Orama's: Duplicate to edit. */
function HeaderActions({ agent, own, onEdit }: { agent: Row; own: boolean; onEdit: () => void }) {
  const scope = useScope();
  const ws = useWsPath();
  const navigate = useNavigate();
  const toast = useToast();
  const listed = useEngineRead<{ agents?: Row[] }>("list_agents");
  const [busy, setBusy] = useState(false);
  const id = text(agent, "id");
  const duplicate = async () => {
    setBusy(true);
    try {
      const taken = (listed.data?.agents ?? []).map((a) => text(a, "name"));
      const made = await act<Row>("create_agent", { customer_id: scope.customerId, workspace_id: scope.workspaceId, ...duplicateInput(agent, taken) });
      toast(`Duplicated as ${text(made, "name")}`);
      navigate(ws(`/design/agent?${qs({ id: text(made, "id") })}`));
    } catch (e) {
      toast((e as Error).message, "bad");
    } finally {
      setBusy(false);
    }
  };
  if (!own) {
    return (
      <Button variant="primary" loading={busy} onClick={() => void duplicate()}>
        <Copy size={14} aria-hidden /> Duplicate to edit
      </Button>
    );
  }
  return (
    <>
      <Button onClick={onEdit}>
        <Pencil size={14} aria-hidden /> Edit
      </Button>
      <Button loading={busy} onClick={() => void duplicate()}>
        <Copy size={14} aria-hidden /> Duplicate
      </Button>
      <ConfirmButton
        variant="ghost"
        danger
        title="Delete This Agent?"
        confirmLabel="Delete agent"
        typed={text(agent, "name")}
        confirm={<>The agent {text(agent, "name")} is deleted from this workspace, and its schedules and the streams that start it with it. Its running services and jobs are stopped. Its revisions are kept.</>}
        onConfirm={async () => {
          await deleteAgent(scope, id);
          toast(`Deleted ${text(agent, "name")}`);
          navigate(ws("/design/agents"));
        }}
      >
        <Trash2 size={14} aria-hidden /> Delete
      </ConfirmButton>
      <Button variant="primary" to={ws(`/train/agent?${qs({ id, tab: "runs" })}`)}>
        <Play size={14} aria-hidden /> Run it
      </Button>
    </>
  );
}

function AgentDesign({ agent, own }: { agent: Row; own: boolean }) {
  const ws = useWsPath();
  const id = text(agent, "id");
  const area = agent.work_area && typeof agent.work_area === "object" ? (agent.work_area as Row) : {};
  let areaText = text(area, "kind") === "project" ? "One project" : text(area, "kind") === "dataset" ? "Specific datasets" : "Anywhere in this workspace";
  if (text(area, "detail")) areaText += `: ${text(area, "detail")}`;
  const skills = (Array.isArray(agent.skills) ? agent.skills : []).filter((s: unknown): s is string => typeof s === "string");
  const instructions = text(agent, "manifesto") || text(agent, "system_prompt") || text(agent.definition, "instructions");
  return (
    <Split
      side={
        <>
          <Panel label="Source" title={<Badge tone={own ? "success" : "info"}>{own ? "Yours" : "By Orama"}</Badge>}>
            <KeyValue
              rows={[
                ["From", own ? "This workspace, editable" : "Orama, read-only"],
                ["Created", <When key="c" value={text(agent, "created_at")} />],
                ["Edited", <When key="u" value={text(agent, "updated_at")} />],
                ["Id", <Mono key="id" short={12}>{id}</Mono>],
              ]}
            />
          </Panel>
          <Panel title="Run and Train It">
            <p className="mt-0 text-[13px] text-ink-2">Running it (once, on a schedule or launched) and its unattended runs are on its Train page.</p>
            <div className="flex flex-col gap-2">
              <Button to={ws(`/train/agent?${qs({ id, tab: "runs" })}`)}>
                <Play size={14} aria-hidden /> Runs and schedules
              </Button>
              {trainable(agent) ? (
                <Button to={ws(`/train/agent?${qs({ id, tab: "training" })}`)}>
                  <GraduationCap size={14} aria-hidden /> Train and improve it
                </Button>
              ) : (
                <p className="muted m-0 text-[12.5px]">Nothing to train: it runs on a frontier model and calls no pipeline.</p>
              )}
              <Button to={`/ask?${qs({ agent: id })}`}>
                <MessageSquare size={14} aria-hidden /> Talk in Ask Orama
              </Button>
            </div>
          </Panel>
        </>
      }
    >
      <Panel title="Definition">
        <KeyValue
          rows={[
            ["What it's for", text(agent, "purpose") || text(agent, "description")],
            ["Instructions", instructions ? <span key="i" className="whitespace-pre-wrap">{instructions}</span> : null],
            ["What success looks like", text(agent, "goal")],
            [
              "Skills",
              skills.length ? (
                <span key="s" className="flex flex-wrap gap-1">
                  {skills.map((s) => (
                    <Chip key={s}>{skillLabel(s)}</Chip>
                  ))}
                </span>
              ) : (
                "None granted"
              ),
            ],
            ["Model", modelLabel(agent) ? <Mono key="m">{modelLabel(agent)}</Mono> : null],
            ["Runs", runsWhere(agent)],
            ["Where it works", areaText],
            ["Runs on its own", text(agent, "trigger") || "No: only in conversation"],
          ]}
        />
      </Panel>
      <WhereUsed agent={agent} />
      {own && <Revisions agent={agent} />}
    </Split>
  );
}

/** Where an agent is used: the pipelines it calls as tools, Lake's schedules for it, the workspace
 * streams that start it, its runs, and Ask Orama. */
function WhereUsed({ agent }: { agent: Row }) {
  const ws = useWsPath();
  const id = text(agent, "id");
  const schedules = useScopedRead<{ schedules?: Row[] }>("list_agent_schedules", { agent_id: id });
  const streams = useScopedRead<{ streams?: Row[] }>("list_agent_streams", { agent_id: id });
  const launches = useScopedRead<{ launches?: Row[] }>("list_agent_launches", { agent_id: id, limit: 20 });
  const pipelines = pipelinesOf(agent);
  const scheduleCount = schedules.data?.schedules?.length ?? 0;
  const runCount = launches.data?.launches?.length ?? 0;
  return (
    <Panel title="Where It Is Used">
      <ul className="m-0 list-none divide-y divide-hairline p-0 text-[13px]">
        <li className="py-2">
          <div className="label m-0">Pipelines it calls as tools</div>
          {pipelines.length ? (
            <span className="flex flex-wrap gap-x-3 gap-y-1">
              {pipelines.map((p) => (
                <span key={`${p.project}/${p.graph}`} className="inline-flex items-center gap-1">
                  <Workflow size={13} className="text-ink-3" aria-hidden />
                  <Link to={ws(`/design/pipeline?${qs({ project: p.project, graph: p.graph })}`)}>{p.graph}</Link>
                  <span className="muted">in {p.project}</span>
                  {p.version && <Chip tone="ok">approved {p.version}</Chip>}
                </span>
              ))}
            </span>
          ) : (
            <span className="muted">None. Grant an approved pipeline in Edit.</span>
          )}
        </li>
        <li className="py-2">
          <div className="label m-0">Schedules</div>
          {schedules.isError ? (
            <span className="muted">Could not be read.</span>
          ) : (
            <Link to={ws(`/train/agent?${qs({ id, tab: "schedules" })}`)}>{scheduleCount === 1 ? "1 schedule" : `${scheduleCount} schedules`}</Link>
          )}
        </li>
        <li className="py-2">
          <div className="label m-0">Streams that start it</div>
          {streams.isError ? (
            <span className="muted">Could not be read.</span>
          ) : streams.data?.streams?.length ? (
            <span className="flex flex-col gap-1">
              {streams.data.streams.map((s) => (
                <span key={text(s, "id")} className="inline-flex flex-wrap items-center gap-1">
                  <Radio size={13} className="text-ink-3" aria-hidden />
                  <Link to={ws(`/train/stream?${qs({ zetta: "workspace", topic: text(s, "stream") })}`)}>{text(s, "stream")}</Link>
                  <span className="muted">
                    each new message starts a run{text(s, "set_by_name") ? `, as ${text(s, "set_by_name")}` : ""}
                  </span>
                </span>
              ))}
            </span>
          ) : (
            <span className="muted">{streams.isLoading ? "Reading…" : "None. Choose streams in Edit."}</span>
          )}
        </li>
        <li className="py-2">
          <div className="label m-0">Unattended runs</div>
          {launches.isError ? (
            <span className="muted">Could not be read.</span>
          ) : (
            <Link to={ws(`/train/agent?${qs({ id, tab: "runs" })}`)}>{runCount >= 20 ? "20 or more recent runs" : runCount === 1 ? "1 recent run" : `${runCount} recent runs`}</Link>
          )}
        </li>
        <li className="py-2">
          <div className="label m-0">Ask Orama</div>
          {agent.is_default === true ? "Ask Orama's default agent in this workspace." : "Anyone here can talk to it in Ask Orama, and Ask Orama can hand it work."}
        </li>
      </ul>
    </Panel>
  );
}

/** Every revision of the definition, newest first, each with what changed and Restore. */
function Revisions({ agent }: { agent: Row }) {
  const scope = useScope();
  const toast = useToast();
  const id = text(agent, "id");
  const revisions = useScopedRead<{ revisions?: Row[]; total?: number }>("list_agent_revisions", { agent_id: id });
  const rows = revisions.data?.revisions ?? [];
  return (
    <Panel title="Revisions" label={revisions.data?.total ? `${revisions.data.total} in all` : undefined}>
      <p className="muted mt-0 text-[12.5px]">Each save is a revision. Restoring one saves its definition again as the newest.</p>
      <DataTable
        q={revisions}
        what="the revisions"
        rows={rows}
        empty={
          <Empty compact icon={History} title="No Revisions Kept Yet">
            Lake keeps a revision each time this agent is saved; it was created before revisions were kept.
          </Empty>
        }
        rowKey={(r) => text(r, "id")}
        columns={[
          { key: "revision", label: "Revision", render: (r) => <span className="font-mono">#{text(r, "revision")}</span> },
          {
            key: "changed",
            label: "What changed",
            render: (r) => {
              const older = rows[rows.indexOf(r) + 1];
              const changes = text(r, "change") === "create" ? ["Created"] : revisionChanges(older?.definition, r.definition ?? {});
              return <span className="text-ink-2">{changes.length ? changes.join(", ") : "Saved unchanged"}</span>;
            },
          },
          { key: "by", label: "By", render: (r) => text(r, "created_by_name") || <Mono short={10}>{text(r, "created_by")}</Mono> },
          { key: "when", label: "When", render: (r) => <When value={text(r, "created_at")} /> },
          {
            key: "restore",
            label: "",
            className: "text-right",
            render: (r) =>
              rows.indexOf(r) === 0 ? (
                <Chip tone="ok">Current</Chip>
              ) : (
                <ConfirmButton
                  variant="ghost"
                  title={`Restore Revision #${text(r, "revision")}?`}
                  confirmLabel="Restore"
                  confirm={<>Its definition is saved again as the newest revision. The revisions after it are kept.</>}
                  onConfirm={async () => {
                    await restoreRevision(scope, id, r.definition ?? {});
                    toast(`Restored revision #${text(r, "revision")}`);
                  }}
                >
                  Restore
                </ConfirmButton>
              ),
          },
        ]}
      />
    </Panel>
  );
}
