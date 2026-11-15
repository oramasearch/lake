/** One MCP connection (`/w/<id>/mcp-connection?id=<id>`, lake#652, docs/ui/mcp-connections.md): its
 * server and status, its tools (allow each, say whether it changes something, try it, auto-approve
 * a change for one agent), the switch that exposes its allowed tools through Lake's own MCP (with
 * what that means), its agent and streams, its recent calls from the audit, and what
 * the server also offers. A server Lake runs here (lake#659) also shows its App: its state, Start,
 * Stop and Restart, and its logs; its tools are discovered once the App is ready. Refresh asks the
 * server again; Delete names what goes. */
import { useEffect, useMemo, useRef, useState } from "react";
import { useNavigate, useSearchParams } from "react-router-dom";
import { Activity, Bot, Box, CalendarClock, Plug, RefreshCw, ShieldCheck, Wrench } from "lucide-react";
import { act, ApiError, useList, useRead, type Row } from "@/lib/api";
import { date, dateTime } from "@/lib/format";
import { Banner, Button, Card, Chip, DataTable, Empty, ErrorBlock, Field, KeyValue, Page, Section, Skeleton, Stat, StatRow } from "@/lib/ui";
import { ConfirmButton, Modal } from "@/lib/overlay";
import { useToast } from "@/lib/toast";
import { useTrail } from "@/lib/trail";
import { WorkspaceGate } from "@/components/WorkspaceGate";
import { qs, useSpaceRead, useWorkspace, useWq, useWsPath } from "@/pages/train/workspace";
import { authLabel, exportWarning, runtimeLabel, statusLabel, stdioCommandLine, transportLabel, viaLabel, type McpConnection, type McpTool } from "./logic";
import { AppPhase } from "@/pages/apps/AppsPage";
import { Terminal } from "@/pages/apps/shared";
import { TryTool } from "./TryTool";

export default function McpConnectionPage() {
  const [params] = useSearchParams();
  const id = params.get("id") ?? "";
  const wsPath = useWsPath();
  const q = useSpaceRead<McpConnection>(id ? "get_mcp_connection" : null, { id });
  useTrail([{ label: "MCP Connections", to: wsPath("/mcp-connections") }, { label: q.data?.name || "MCP Connection" }]);
  return (
    <WorkspaceGate>
      {!id ? (
        <Page title="MCP Connection">
          <ErrorBlock error={new ApiError(404, "not_found", "No connection was named. Open one from MCP Connections.")} />
        </Page>
      ) : q.isLoading ? (
        <div className="page-container">
          <Skeleton lines={6} />
        </div>
      ) : q.isError || !q.data ? (
        <Page title="MCP Connection">
          <ErrorBlock error={q.error} retry={() => void q.refetch()} what="this connection" />
        </Page>
      ) : (
        <ConnectionView c={q.data} />
      )}
    </WorkspaceGate>
  );
}

function ConnectionView({ c }: { c: McpConnection }) {
  const navigate = useNavigate();
  const toast = useToast();
  const wsPath = useWsPath();
  const scope = useWorkspace();
  const base = { customer_id: scope.customerId, workspace_id: scope.workspaceId };
  const [refreshing, setRefreshing] = useState(false);
  const [signingIn, setSigningIn] = useState(false);
  const [trying, setTrying] = useState<McpTool | null>(null);
  const tools = c.tools ?? [];
  const serverName = c.server?.name || c.name;
  const stdio = c.kind === "stdio" && c.stdio ? c.stdio : null;

  const refresh = async () => {
    setRefreshing(true);
    try {
      const next = await act<McpConnection>("refresh_mcp_connection", { ...base, id: c.id });
      toast(next.status === "connected" ? `${next.tool_count} tools from ${next.name}` : statusLabel(next.status));
    } catch (e) {
      toast((e as Error).message);
    } finally {
      setRefreshing(false);
    }
  };

  const actions = (
    <>
      {c.auth.kind === "oauth" && (
        <Button variant={c.status === "needs_sign_in" ? "primary" : "secondary"} onClick={() => setSigningIn(true)}>
          {c.auth.signed_in_at ? "Sign in again" : "Sign in"}
        </Button>
      )}
      <Button onClick={() => void refresh()} loading={refreshing}>
        <RefreshCw size={14} aria-hidden /> Refresh
      </Button>
      <ConfirmButton
        variant="danger"
        title={`Delete ${c.name}?`}
        confirmLabel="Delete connection"
        typed={c.name}
        confirm={
          <>
            People, Ask Orama and agents lose its tools at once, and its agent loses its only tools.{" "}
            {stdio ? "Its App is deleted with it. " : ""}The secret it used stays in its mount.
          </>
        }
        onConfirm={async () => {
          await act("delete_mcp_connection", { ...base, id: c.id });
          toast(`${c.name} deleted`);
          navigate(wsPath("/mcp-connections"));
        }}
      >
        Delete
      </ConfirmButton>
    </>
  );

  return (
    <Page eyebrow="MCP Connection" title={c.name} sub={<span className="font-mono text-[12.5px] break-all">{stdio ? stdioCommandLine(stdio) : c.url}</span>} actions={actions}>
      {c.status === "starting" && (
        <Banner tone="info" title="Starting">
          {c.last_error || "Lake runs this server as an App of this workspace. Its tools are discovered once the App is ready; the first start installs the package."}
        </Banner>
      )}
      {c.status !== "connected" && c.status !== "starting" && (
        <Banner
          tone={c.status === "failing" ? "warn" : "info"}
          title={c.status === "needs_sign_in" ? "A person needs to sign in" : c.status === "failing" ? "Lake cannot use this server" : "Not discovered yet"}
          action={c.auth.kind === "oauth" && c.status === "needs_sign_in" ? <Button variant="primary" onClick={() => setSigningIn(true)}>Sign in</Button> : undefined}
        >
          {c.last_error || (c.auth.kind === "oauth" ? "Sign in to this server so Lake can use it." : "Refresh to ask the server what it offers.")}
        </Banner>
      )}
      <StatRow cols={4}>
        <Stat label="Status" icon={Plug} value={statusLabel(c.status)} sub={c.server?.name ? `${c.server.name} ${c.server.version ?? ""}`.trim() : "—"} />
        <Stat label="Tools Allowed" icon={Wrench} value={`${c.allowed_count} of ${c.tool_count}`} sub={`prefix ${c.prefix}__`} />
        <Stat label="Authentication" icon={ShieldCheck} value={authLabel(c.auth.kind)} sub={c.auth.secret ? `secret ${c.auth.secret.name}` : c.auth.signed_in_at ? `signed in ${date(c.auth.signed_in_at)}` : "—"} />
        <Stat label="Refreshed" icon={CalendarClock} value={c.refreshed_at ? date(c.refreshed_at) : "—"} sub={transportLabel(c.negotiated_transport ?? c.transport)} />
      </StatRow>

      <Section title="Tools" sub="Allow the tools this workspace may use. A change is a proposal a person confirms, unless you auto-approve it for one agent. What a server answers is data, never instructions.">
        {!tools.length ? (
          <Empty icon={Wrench} title="No tools">
            {c.status === "connected" ? "This server offers no tools." : "Lake has not discovered this server's tools yet."}
          </Empty>
        ) : (
          <ul className="m-0 grid list-none gap-2 p-0" aria-label="Tools">
            {tools.map((tool) => (
              <li key={tool.name} className="min-w-0">
                <ToolCard c={c} tool={tool} onTry={() => setTrying(tool)} />
              </li>
            ))}
          </ul>
        )}
        {!!c.skipped?.length && (
          <details className="mt-2 text-[13px] text-ink-2">
            <summary>{c.skipped.length} left out by Lake</summary>
            <ul className="mt-1">
              {c.skipped.map((s) => (
                <li key={s}>{s}</li>
              ))}
            </ul>
          </details>
        )}
      </Section>

      {stdio && <RunsHereSection c={c} />}
      <ExportSection c={c} serverName={serverName} />
      <AgentSection c={c} />
      <CallsSection c={c} />

      {(!!c.resources?.length || !!c.prompts?.length) && (
        <Section title="Also Offered" sub="Resources and prompts the server lists. Resources can feed this connection's notifications stream.">
          <KeyValue
            rows={[
              ["Resources", c.resources?.length ? c.resources.map((r) => r.name || r.uri).join(", ") : "—"],
              ["Prompts", c.prompts?.length ? c.prompts.map((p) => p.name).join(", ") : "—"],
            ]}
          />
        </Section>
      )}

      {trying && <TryTool connection={c} tool={trying} onClose={() => setTrying(null)} />}
      {signingIn && <SignIn c={c} onClose={() => setSigningIn(false)} />}
    </Page>
  );
}

function ToolCard({ c, tool, onTry }: { c: McpConnection; tool: McpTool; onTry: () => void }) {
  const scope = useWorkspace();
  const toast = useToast();
  const [busy, setBusy] = useState(false);
  const set = async (change: Partial<Pick<McpTool, "allowed" | "change">>) => {
    setBusy(true);
    try {
      await act("update_mcp_connection", { customer_id: scope.customerId, workspace_id: scope.workspaceId, id: c.id, tools: { [tool.name]: change } });
    } catch (e) {
      toast((e as Error).message);
    } finally {
      setBusy(false);
    }
  };
  return (
    <Card padded={false} tone={tool.allowed ? "default" : "quiet"}>
      <div className="px-4 py-3">
        <div className="flex min-w-0 flex-wrap items-center gap-x-2 gap-y-1">
          <span className="min-w-0 flex-1 truncate text-[14px] font-semibold text-ink">{tool.title || tool.name}</span>
          <Chip tone={tool.change ? "warn" : "neutral"}>{tool.change ? "Change" : "Read"}</Chip>
          {tool.allowed ? <Chip tone="ok">Allowed</Chip> : <Chip>Not allowed</Chip>}
        </div>
        <div className="mt-0.5 font-mono text-[11.5px] text-ink-3 break-all">{tool.exported_name}</div>
        {tool.description && <p className="mt-1 mb-0 line-clamp-3 text-[13px] text-ink-2">{tool.description}</p>}
        <div className="mt-2 flex flex-wrap items-center gap-3">
          <label className="inline-flex items-center gap-1.5 text-[13px]">
            <input type="checkbox" checked={tool.allowed} disabled={busy} onChange={(e) => void set({ allowed: e.target.checked })} /> Allowed
          </label>
          <label className="inline-flex items-center gap-1.5 text-[13px]" title={tool.read_only_hint === true ? "The server says it only reads" : "The server does not say it only reads"}>
            <input type="checkbox" checked={tool.change} disabled={busy} onChange={(e) => void set({ change: e.target.checked })} /> Changes something
          </label>
          <span className="flex-1" />
          <Button onClick={onTry} disabled={!tool.allowed} title={tool.allowed ? undefined : "Allow it first"}>
            Try it
          </Button>
        </div>
        {tool.allowed && tool.change && <AutoApprove c={c} tool={tool} />}
      </div>
    </Card>
  );
}

/** One change tool's per-agent grants: who may run it unattended, and adding or taking one back. */
function AutoApprove({ c, tool }: { c: McpConnection; tool: McpTool }) {
  const scope = useWorkspace();
  const toast = useToast();
  const agents = useWq<{ agents?: Row[] }>("list_agents", {}, { staleTime: 60_000 });
  const [agent, setAgent] = useState("");
  const grant = async (who: string, approve: boolean) => {
    try {
      await act("set_mcp_tool_auto_approve", { customer_id: scope.customerId, workspace_id: scope.workspaceId, connection_id: c.id, tool: tool.name, agent: who, auto_approve: approve });
      setAgent("");
      toast(approve ? `Auto-approved for ${who}` : `No longer auto-approved for ${who}`);
    } catch (e) {
      toast((e as Error).message);
    }
  };
  const granted = tool.auto_approved ?? [];
  const choices = (agents.data?.agents ?? []).filter((a) => a.runtime === "python" && !granted.some((g) => g.agent === a.id || g.agent === a.name));
  return (
    <div className="mt-2 rounded-[4px] border border-hairline px-3 py-2 text-[12.5px]">
      <div className="text-ink-2">
        Auto-approved for unattended agents: an agent listed here runs this change without a person confirming each call. Each call is audited.
      </div>
      <div className="mt-1.5 flex flex-wrap items-center gap-1.5">
        {granted.length ? (
          granted.map((g) => (
            <Chip key={g.agent} tone="ai" onClick={() => void grant(g.agent, false)} title={`Granted by ${g.granted_by ?? "someone"}${g.granted_at ? ` on ${date(g.granted_at)}` : ""}. Press to take it back.`}>
              {(agents.data?.agents ?? []).find((a) => a.id === g.agent || a.name === g.agent)?.name ?? g.agent} ✕
            </Chip>
          ))
        ) : (
          <span className="text-ink-3">None: every call is a proposal.</span>
        )}
        <select aria-label="Agent to auto-approve" value={agent} onChange={(e) => setAgent(e.target.value)} className="max-w-[220px]">
          <option value="">Add an agent…</option>
          {choices.map((a) => (
            <option key={a.id} value={a.id}>
              {a.name}
            </option>
          ))}
        </select>
        <Button onClick={() => void grant(agent, true)} disabled={!agent}>
          Auto-approve
        </Button>
      </div>
    </div>
  );
}

function ExportSection({ c, serverName }: { c: McpConnection; serverName: string }) {
  const scope = useWorkspace();
  const toast = useToast();
  const set = async (exported: boolean) => {
    await act("update_mcp_connection", { customer_id: scope.customerId, workspace_id: scope.workspaceId, id: c.id, exported });
    toast(exported ? "Exposed through Lake's MCP" : "No longer exposed through Lake's MCP");
  };
  return (
    <Section title="Through Lake's MCP" sub={<>Off by default. When on, this connection's allowed tools appear in this workspace's MCP endpoint as <code>{c.prefix}__&lt;tool&gt;</code>, so one MCP client set up for Lake reaches them too.</>}>
      <Card>
        <div className="flex flex-wrap items-center gap-3">
          <div className="min-w-0 flex-1 text-[13.5px]">
            {c.exported ? (
              <>
                <strong>On.</strong> {exportWarning(serverName)}
              </>
            ) : (
              <>
                <strong>Off.</strong> Only this workspace's people, Ask Orama and its agents use these tools, through Lake.
              </>
            )}
          </div>
          {c.exported ? (
            <Button onClick={() => void set(false).catch((e) => toast((e as Error).message))}>Turn off</Button>
          ) : (
            <ConfirmButton variant="secondary" title="Expose these tools through Lake's MCP?" confirmLabel="Turn on" confirm={exportWarning(serverName)} onConfirm={() => set(true)}>
              Turn on
            </ConfirmButton>
          )}
        </div>
      </Card>
    </Section>
  );
}

function AgentSection({ c }: { c: McpConnection }) {
  const scope = useWorkspace();
  const toast = useToast();
  const wsPath = useWsPath();
  const [prompt, setPrompt] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<unknown>(null);
  const streams = c.streams ?? {};
  const [input, setInput] = useState(streams.input_topic ?? "");
  const [output, setOutput] = useState(streams.output_topic ?? "");
  const [notifications, setNotifications] = useState(streams.notifications_topic ?? "");
  const [resources, setResources] = useState<string[]>(streams.subscribe_resources ?? []);
  const topics = useList<Row>("list_streams", { customer_id: scope.customerId, workspace_id: scope.workspaceId, limit: 200 }, { pick: "groups" });
  const topicNames = useMemo(() => (topics.data ?? []).filter((g) => g.zetta === "workspace").flatMap((g) => (g.topics ?? []).map((t: Row) => t.topic ?? t.name)).filter(Boolean), [topics.data]);

  const create = async () => {
    setBusy(true);
    setError(null);
    try {
      await act("create_mcp_connection_agent", { customer_id: scope.customerId, workspace_id: scope.workspaceId, id: c.id, prompt });
      setPrompt("");
      toast("Agent created");
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };
  const saveStreams = async () => {
    setBusy(true);
    setError(null);
    try {
      await act("update_mcp_connection", {
        customer_id: scope.customerId,
        workspace_id: scope.workspaceId,
        id: c.id,
        input_topic: input.trim() || null,
        output_topic: output.trim() || null,
        notifications_topic: notifications.trim() || null,
        subscribe_resources: resources,
      });
      toast("Streams saved");
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };

  return (
    <Section title="Agent and Streams" sub="Optional. An agent whose only tools are this connection's; events on an input stream start its turns, its results go to an output stream, and the server's notifications can be mirrored to a stream with no agent at all.">
      <Card>
        <div className="stack">
          {c.agent_id ? (
            <div className="flex flex-wrap items-center gap-2 text-[13.5px]">
              <Bot size={16} aria-hidden />
              <span className="min-w-0 flex-1">This connection has an agent. Its turns act with the authority of the person who set its streams.</span>
              <Button to={wsPath(`/train/agent?${qs({ id: c.agent_id })}`)}>Open the agent</Button>
            </div>
          ) : (
            <Field label="What its agent is for" help="Its prompt. It gets the skill mcp:<prefix> and no other tools.">
              <textarea rows={3} value={prompt} onChange={(e) => setPrompt(e.target.value)} placeholder="Triage new tickets: read each one and propose a priority." />
            </Field>
          )}
          {!c.agent_id && (
            <div>
              <Button onClick={() => void create()} loading={busy} disabled={!prompt.trim()}>
                <Bot size={14} aria-hidden /> Create an agent for this connection
              </Button>
            </div>
          )}
          <datalist id={`topics-${c.id}`}>
            {topicNames.map((t: string) => (
              <option key={t} value={t} />
            ))}
          </datalist>
          <div className="grid gap-3 md:grid-cols-3">
            <Field label="Input stream" help={c.agent_id ? "Each event starts a turn of its agent." : "Needs an agent."}>
              <input list={`topics-${c.id}`} value={input} onChange={(e) => setInput(e.target.value)} disabled={!c.agent_id} placeholder="tickets-in" autoComplete="off" />
            </Field>
            <Field label="Output stream" help="Each turn's result, as agent-events carries it.">
              <input list={`topics-${c.id}`} value={output} onChange={(e) => setOutput(e.target.value)} disabled={!c.agent_id} placeholder="tickets-out" autoComplete="off" />
            </Field>
            <Field label="Notifications stream" help="The server's notifications, mirrored.">
              <input list={`topics-${c.id}`} value={notifications} onChange={(e) => setNotifications(e.target.value)} placeholder="tickets-notifications" autoComplete="off" />
            </Field>
          </div>
          {!!c.resources?.length && (
            <fieldset className="m-0 border-0 p-0">
              <legend className="field-label">Subscribe to resources</legend>
              <div className="flex flex-wrap gap-3">
                {c.resources.map((r) => (
                  <label key={r.uri} className="inline-flex items-center gap-1.5 text-[13px]">
                    <input type="checkbox" checked={resources.includes(r.uri)} onChange={(e) => setResources((prev) => (e.target.checked ? [...prev, r.uri] : prev.filter((u) => u !== r.uri)))} />
                    {r.name || r.uri}
                  </label>
                ))}
              </div>
            </fieldset>
          )}
          {error ? <p className="error m-0">{(error as Error).message}</p> : null}
          <div>
            <Button onClick={() => void saveStreams()} loading={busy}>
              Save streams
            </Button>
          </div>
        </div>
      </Card>
    </Section>
  );
}

function CallsSection({ c }: { c: McpConnection }) {
  const calls = useSpaceRead<{ calls?: Row[] }>("list_mcp_connection_calls", { id: c.id, limit: 25 });
  const rows = (calls.data?.calls ?? []).filter((r) => r.action === "call_mcp_tool" || r.action === "read_mcp_tool");
  return (
    <Section title="Recent Calls" sub="From this workspace's audit, newest first. Arguments and results are never recorded.">
      {calls.isLoading ? (
        <Skeleton lines={3} />
      ) : calls.isError ? (
        <ErrorBlock error={calls.error} retry={() => void calls.refetch()} what="recent calls" />
      ) : !rows.length ? (
        <Empty icon={Activity} title="No calls yet">
          Calls made by people, Ask Orama, agents and Lake's MCP appear here.
        </Empty>
      ) : (
        <DataTable
          rows={rows}
          rowKey={(r: Row) => `${r.at}-${r.tool}-${r.principal}`}
          columns={[
            { key: "at", label: "When", render: (r: Row) => dateTime(r.at) },
            { key: "tool", label: "Tool", render: (r: Row) => <span className="font-mono text-[12px]">{r.tool}</span> },
            { key: "via", label: "Through", render: (r: Row) => viaLabel(r.via, r.agent) + (r.auto_approved ? " (auto-approved)" : "") },
            { key: "outcome", label: "Outcome", render: (r: Row) => <Chip tone={r.outcome === "success" && !r.is_error ? "ok" : "bad"}>{r.outcome === "success" ? (r.is_error ? "Tool error" : "Done") : r.error_code === "sign_in_required" ? "Needs sign-in" : r.error_code === "not_approved" ? "Not approved" : (r.error_code || r.outcome).replace(/_/g, " ")}</Chip> },
          ]}
        />
      )}
    </Section>
  );
}

/** Sign in to an OAuth server: choose the secret mount the refresh token goes to, then Lake
 * answers the authorization URL and the browser goes there; the server sends it back to
 * /mcp-connections/oauth/callback. */
function SignIn({ c, onClose }: { c: McpConnection; onClose: () => void }) {
  const scope = useWorkspace();
  const mounts = useList<Row>("list_secret_mounts", { customer_id: scope.customerId, workspace_id: scope.workspaceId, limit: 200 }, { pick: "items" });
  const [mountId, setMountId] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<unknown>(null);
  const begin = async () => {
    setBusy(true);
    setError(null);
    try {
      const started = await act<{ authorization_url: string }>("begin_mcp_connection_sign_in", { customer_id: scope.customerId, workspace_id: scope.workspaceId, id: c.id, mount_id: mountId });
      window.location.assign(started.authorization_url);
    } catch (e) {
      setError(e);
      setBusy(false);
    }
  };
  return (
    <Modal
      title={`Sign In to ${c.server?.name || c.name}`}
      onClose={onClose}
      width={480}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button variant="primary" onClick={() => void begin()} loading={busy} disabled={!mountId}>
            Continue to sign in
          </Button>
        </>
      }
    >
      <div className="stack">
        <p className="muted m-0">
          You sign in at the server. Lake keeps only the refresh token, as the secret <code>mcp-{c.prefix}</code> in the mount you choose, and never shows it to anyone, a model or an agent.
        </p>
        <Field label="Secret mount" required>
          <select value={mountId} onChange={(e) => setMountId(e.target.value)}>
            <option value="">{mounts.isLoading ? "Loading mounts…" : "Choose a mount"}</option>
            {(mounts.data ?? []).map((m) => (
              <option key={m.id} value={m.id}>
                {m.name}
              </option>
            ))}
          </select>
        </Field>
        {!mounts.isLoading && !(mounts.data ?? []).length && <Banner tone="warn">This workspace has no secret mount yet. Make one in Secrets first.</Banner>}
        {error ? <p className="error m-0">{(error as Error).message}</p> : null}
      </div>
    </Modal>
  );
}

/** A server Lake runs here (lake#659): its App's state, Start, Stop and Restart, and its logs. Once
 * the App is ready while the connection is still starting, the page asks Lake to discover it. */
function RunsHereSection({ c }: { c: McpConnection }) {
  const scope = useWorkspace();
  const toast = useToast();
  const wsPath = useWsPath();
  const base = { customer_id: scope.customerId, workspace_id: scope.workspaceId, app_id: c.app_id ?? "" };
  const app = useRead<Row>(c.app_id ? "get_app" : null, base, { refetchInterval: c.status === "starting" ? 5_000 : 30_000 });
  const logs = useRead<{ items?: Row[] }>(c.app_id ? "app_logs" : null, { ...base, limit: 200 });
  const [busy, setBusy] = useState("");
  const discovered = useRef(false);
  const ready = !!app.data?.ready;
  useEffect(() => {
    if (ready && c.status === "starting" && !discovered.current) {
      discovered.current = true;
      void act("refresh_mcp_connection", { customer_id: scope.customerId, workspace_id: scope.workspaceId, id: c.id }).catch(() => {});
    }
  }, [ready, c.status, c.id, scope.customerId, scope.workspaceId]);
  const lifecycle = async (capability: string, done: string) => {
    setBusy(capability);
    try {
      await act(capability, base);
      toast(done);
      void app.refetch();
    } catch (e) {
      toast((e as Error).message);
    } finally {
      setBusy("");
    }
  };
  const stdio = c.stdio!;
  const stopped = app.data?.desired === "stopped";
  return (
    <Section
      title="Runs in This Workspace"
      sub="Lake runs this server as an App of this workspace, metered like any App, and reaches it inside the cluster. Its token and secret variables come from the workspace's secrets."
      actions={
        <div className="flex flex-wrap gap-2">
          {stopped ? (
            <Button onClick={() => void lifecycle("start_app", "Start requested")} loading={busy === "start_app"}>
              Start
            </Button>
          ) : (
            <>
              <Button onClick={() => void lifecycle("restart_app", "Restart requested")} loading={busy === "restart_app"}>
                Restart
              </Button>
              <Button onClick={() => void lifecycle("stop_app", "Stop requested")} loading={busy === "stop_app"}>
                Stop
              </Button>
            </>
          )}
        </div>
      }
    >
      <KeyValue
        rows={[
          ["App", app.data ? <AppPhase phase={String(app.data.phase ?? "")} ready={ready} /> : app.isLoading ? "…" : "Not found"],
          ["Package", <span className="font-mono text-[12.5px] break-all">{stdio.package}</span>],
          ["Runtime", runtimeLabel(stdio.runtime)],
          ["Command", <span className="font-mono text-[12.5px] break-all">{stdioCommandLine(stdio)}</span>],
          ["Environment", [...(stdio.env_keys ?? []), ...(stdio.secret_env_keys ?? []).map((k) => `${k} (secret)`)].join(", ") || "—"],
          [
            "Details",
            c.app_id ? (
              <a href={wsPath("/ship/app") + qs({ customer_id: scope.customerId, app_id: c.app_id })}>
                <Box size={13} aria-hidden /> {stdio.app_name || "Open the App"}
              </a>
            ) : (
              "—"
            ),
          ],
        ]}
      />
      {app.data?.message && <p className="muted mt-2 mb-0 text-[13px]">{String(app.data.message)}</p>}
      <Terminal
        className="mt-3"
        title="Logs · last 200 lines"
        empty="No log lines yet."
        actions={
          <button type="button" className="button button-ghost" onClick={() => void logs.refetch()}>
            Refresh
          </button>
        }
      >
        {logs.isError || !Array.isArray(logs.data?.items) ? "" : logs.data!.items!.map((item) => `${String(item.ts ?? "")} ${String(item.line ?? "")}\n`).join("")}
      </Terminal>
    </Section>
  );
}
