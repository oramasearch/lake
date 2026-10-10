/** Ship → Apps (web_ui.cpp web_ui_apps; handlers serve_apps_page and serve_app_form create,
 * agent and agent_confirm): the space's apps as a card grid with their figures, New app (from a
 * repository or an image) in a drawer, and an ApplicationAgent turn with its proposal confirmed
 * against a fresh read of the task. */
import { type FormEvent, type ReactNode, useMemo, useState } from "react";
import { Link, useNavigate, useSearchParams } from "react-router-dom";
import { Bot, Box, ExternalLink, GitBranch, Loader2, Plus, Rocket } from "lucide-react";
import { act, ApiError, useList, type Row } from "@/lib/api";
import { relative } from "@/lib/format";
import { Drawer } from "@/lib/overlay";
import { useSession } from "@/lib/session";
import { Banner, Button, Card, CopyBlock, Empty, ErrorBlock, FilterChips, FormError, Grid, KeyValue, Notice, Page, SearchInput, Skeleton, Stat, StateTabs, StatRow, Status, Toolbar } from "@/lib/ui";
import {
  AgentEventsLink,
  type AgentTask,
  canonicalJson,
  fetchAgentTask,
  Markdown,
  needsReply,
  PageError,
  qs,
  taskReply,
  taskResult,
  taskWorking,
  useAgentDeployed,
  useAgentTask,
  useRunner,
  useWorkspaceLink,
} from "./shared";

/** A form's fields as strings. */
export function formValues(event: FormEvent<HTMLFormElement>): Record<string, string> {
  const values: Record<string, string> = {};
  new FormData(event.currentTarget).forEach((value, key) => {
    values[key] = typeof value === "string" ? value : "";
  });
  return values;
}

/** The form handler's `decimal_value`: a whole decimal number, else 0. */
export function decimal(value: string | undefined): number {
  return /^\d+$/.test((value ?? "").trim()) ? Number(value) : 0;
}

/** An app's phase in words ("Pending" when it has none yet). */
export function phaseWords(phase?: string | null): string {
  return phase || "Pending";
}

/** An app's phase as a state chip: ready is success, Failed danger, Pending warning. */
export function AppPhase({ phase, ready }: { phase?: string | null; ready?: boolean }) {
  const words = phaseWords(phase);
  return <Status value={words} tone={ready ? "success" : words === "Failed" ? "danger" : words === "Pending" ? "warning" : "neutral"} />;
}

/** An app's address: a link when it is http(s), else its text, else "Not assigned". */
export function AppUrl({ url }: { url?: string | null }) {
  if (!url) return <span className="muted">Not assigned</span>;
  if (!/^https?:\/\//.test(url)) return <>{url}</>;
  return (
    <a href={url} rel="noreferrer" className="break-all">
      {url}
    </a>
  );
}

function bindingsSummary(app: Row): string {
  const bindings = (app.bindings ?? {}) as Row;
  const zetta = bindings.zetta !== undefined && bindings.zetta !== null;
  const services = Array.isArray(bindings.services) ? bindings.services.length : 0;
  if (!zetta && services === 0) return "None";
  return `${zetta ? "" : "No "}Zetta, ${services} service${services === 1 ? "" : "s"}`;
}

const SUB = "Create, deploy and operate customer applications with live rollout status.";

export default function AppsPage() {
  const { space, workspaceId } = useSession();
  const ws = useWorkspaceLink();
  const navigate = useNavigate();
  const [params] = useSearchParams();
  const customerId = space?.id ?? "";
  const taskId = params.get("task") ?? "";
  const flash = params.get("error") ?? "";
  const runner = useRunner();
  const [open, setOpen] = useState<"create" | "agent" | null>(null);
  const [mode, setMode] = useState<"runtime" | "image">("runtime");
  const [search, setSearch] = useState("");
  const [phase, setPhase] = useState<string | null>(null);

  const apps = useList<Row>(customerId ? "list_apps" : null, { customer_id: customerId, workspace_id: workspaceId ?? undefined }, { pick: "apps" });
  const repositories = useList<Row>(customerId ? "list_repositories" : null, { customer_id: customerId, limit: 500 }, { pick: "items", workspace: null });
  const agent = useAgentDeployed("agent-application");
  const task = useAgentTask(taskId, customerId, workspaceId);

  const rows = apps.data;
  const phases = useMemo(() => {
    const counts = new Map<string, number>();
    for (const app of rows ?? []) counts.set(phaseWords(app.phase), (counts.get(phaseWords(app.phase)) ?? 0) + 1);
    return [...counts].map(([id, count]) => ({ id, label: id, count }));
  }, [rows]);

  if (!space) {
    return (
      <Page eyebrow="Ship" title="Apps" sub={SUB}>
        <Empty icon={Rocket} title="No workspace yet">
          You have no workspace yet.
        </Empty>
      </Page>
    );
  }

  const close = () => {
    setOpen(null);
    runner.setError(null);
  };

  const create = async (event: FormEvent<HTMLFormElement>) => {
    event.preventDefault();
    const form = formValues(event);
    const input: Row = { customer_id: customerId, workspace_id: workspaceId, name: form.name, mode: form.mode, framework: form.framework, port: decimal(form.port) };
    if (form.mode === "runtime") {
      input.source = { repository: form.repository ?? "", revision: form.revision ?? "", path: form.path ?? "", entrypoint: "" };
    } else {
      input.image = form.image ?? "";
    }
    const created = await runner.run("create", () => act<Row>("create_app", input));
    if (created) navigate(ws("/ship/app") + qs({ customer_id: customerId, app_id: String(created.id ?? "") }));
  };

  const startTurn = async (message: string, conversationId?: string) => {
    const input: Row = { customer_id: customerId, workspace_id: workspaceId, agent: "agent-application", kind: "app", message };
    if (conversationId) input.conversation_id = conversationId;
    const result = await act<Row>("agent_turn", input);
    navigate(ws("/ship/apps") + qs({ customer_id: customerId, task: String(result.task_id ?? "") }));
  };

  const filtered = (rows ?? []).filter((app) => {
    const needle = search.trim().toLowerCase();
    const hay = `${app.name ?? ""} ${app.url ?? ""} ${app.framework ?? ""}`.toLowerCase();
    return (!needle || hay.includes(needle)) && (!phase || phaseWords(app.phase) === phase);
  });
  const all = rows ?? [];
  const ready = all.filter((app) => app.ready).length;
  const failed = all.filter((app) => app.phase === "Failed").length;
  const withZetta = all.filter((app) => app.bindings?.zetta !== undefined && app.bindings?.zetta !== null).length;
  const appHref = (app: Row) => ws("/ship/app") + qs({ customer_id: customerId, app_id: String(app.id) });

  return (
    <Page
      eyebrow="Ship"
      title="Apps"
      sub={SUB}
      actions={
        <>
          <Button onClick={() => setOpen("agent")}>
            <Bot size={14} aria-hidden /> Build with ApplicationAgent
          </Button>
          <Button variant="primary" onClick={() => setOpen("create")}>
            <Plus size={14} aria-hidden /> New app
          </Button>
        </>
      }
    >
      {flash && !runner.error && <Banner tone="warn">{flash}</Banner>}
      {!open && <PageError error={runner.error} />}
      {taskId && <TaskPanels taskId={taskId} q={task} customerId={customerId} runner={runner} startTurn={startTurn} />}

      {apps.isLoading ? (
        <Skeleton lines={4} />
      ) : apps.isError ? (
        <ErrorBlock error={apps.error} retry={() => void apps.refetch()} what="apps" />
      ) : all.length === 0 ? (
        <Empty
          icon={Rocket}
          title="No apps yet"
          action={
            <Button variant="primary" onClick={() => setOpen("create")}>
              <Plus size={14} aria-hidden /> New app
            </Button>
          }
        >
          Create one from a Lake repository or a container image.
        </Empty>
      ) : (
        <>
          <StatRow>
            <Stat label="Apps" value={all.length} icon={Box} />
            <Stat label="Ready" value={ready} tone={ready ? "ok" : undefined} sub={`of ${all.length}`} />
            <Stat label="Failed" value={failed} tone={failed ? "bad" : undefined} />
            <Stat label="With Zetta" value={withZetta} sub="bound to the workspace Zetta" />
          </StatRow>
          <Toolbar>
            <SearchInput value={search} onChange={setSearch} placeholder="Search apps…" label="Search apps" />
            <FilterChips options={phases} value={phase} onChange={setPhase} />
          </Toolbar>
          {filtered.length === 0 ? (
            <Empty compact title="No app matches these filters">
              Clear a filter to see more.
            </Empty>
          ) : (
            <Grid>
              {filtered.map((app) => (
                <Card
                  key={String(app.id)}
                  label={app.mode === "runtime" ? "From a repository" : "From an image"}
                  title={
                    <Link to={appHref(app)} className="text-ink no-underline hover:underline">
                      {String(app.name ?? "")}
                    </Link>
                  }
                  actions={<AppPhase phase={app.phase} ready={!!app.ready} />}
                  footer={
                    <span className="flex flex-wrap items-center justify-between gap-2">
                      <span>Updated {relative(app.updated_at, Date.now(), "—")}</span>
                      <Link to={appHref(app)}>Open</Link>
                    </span>
                  }
                >
                  <div className="mb-2 truncate text-[13px]">
                    <AppUrl url={app.url} />
                  </div>
                  <KeyValue
                    className="text-[13px]"
                    rows={[
                      ["Revision", <code key="r">{app.mode === "runtime" ? String(app.source?.revision ?? "") : String(app.image ?? "")}</code>],
                      ["Bindings", bindingsSummary(app)],
                    ]}
                  />
                </Card>
              ))}
            </Grid>
          )}
        </>
      )}

      {open === "create" && (
        <Drawer
          title="New App"
          subtitle="Run an app from a Lake repository or a container image."
          onClose={close}
          footer={
            <>
              <Button onClick={close}>Cancel</Button>
              <Button variant="primary" type="submit" form={`create-${mode}`} loading={runner.busy === "create"}>
                Create app
              </Button>
            </>
          }
        >
          <StateTabs
            value={mode}
            onChange={setMode}
            label="Source"
            tabs={[
              { value: "runtime", label: <><GitBranch size={14} aria-hidden /> From a repository</> },
              { value: "image", label: <><Box size={14} aria-hidden /> From an image</> },
            ]}
          />
          <FormError error={runner.error} />
          {mode === "runtime" ? (
            <form id="create-runtime" className="stack" onSubmit={create}>
              <input type="hidden" name="mode" value="runtime" />
              <label>
                Name
                <input name="name" required pattern="[a-z][a-z0-9-]{0,30}" />
              </label>
              <label>
                Repository
                <select name="repository" required>
                  {(repositories.data ?? []).map((repository) => (
                    <option key={String(repository.id)} value={String(repository.id)}>
                      {String(repository.name ?? repository.id)}
                    </option>
                  ))}
                </select>
              </label>
              {repositories.isError && <small className="field-error">{(repositories.error as Error).message}</small>}
              <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
                <label>
                  Revision
                  <input name="revision" defaultValue="main" required />
                </label>
                <label>
                  Path
                  <input name="path" defaultValue="." />
                </label>
                <label>
                  Framework
                  <input name="framework" defaultValue="auto" required />
                </label>
                <label>
                  Port
                  <input type="number" name="port" defaultValue="3000" min={1} max={65535} required />
                </label>
              </div>
            </form>
          ) : (
            <form id="create-image" className="stack" onSubmit={create}>
              <input type="hidden" name="mode" value="image" />
              <label>
                Name
                <input name="name" required pattern="[a-z][a-z0-9-]{0,30}" />
              </label>
              <label>
                Image
                <input name="image" placeholder="registry.example/app:tag" required />
              </label>
              <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
                <label>
                  Framework
                  <input name="framework" defaultValue="container" required />
                </label>
                <label>
                  Port
                  <input type="number" name="port" defaultValue="3000" min={1} max={65535} required />
                </label>
              </div>
            </form>
          )}
        </Drawer>
      )}

      {open === "agent" && (
        <Drawer
          title="Build an App With the ApplicationAgent"
          subtitle="Describe the app. The agent proposes files and app settings for you to confirm."
          onClose={close}
          footer={
            <>
              <Button onClick={close}>Cancel</Button>
              <Button variant="primary" type="submit" form="agent-turn" disabled={!agent.deployed} loading={runner.busy === "agent"}>
                Start ApplicationAgent
              </Button>
            </>
          }
        >
          <FormError error={runner.error} />
          <form
            id="agent-turn"
            className="stack"
            onSubmit={(event) => {
              event.preventDefault();
              const form = formValues(event);
              void runner
                .run("agent", async () => {
                  await startTurn(form.description ?? "");
                  return true;
                })
                .then((started) => {
                  if (started) setOpen(null);
                });
            }}
          >
            <label>
              Description
              <textarea name="description" rows={6} placeholder="What should the app do?" disabled={!agent.deployed} />
            </label>
          </form>
          {!agent.deployed && !agent.loading && <Notice tone="warning">ApplicationAgent is not registered on this lake yet.</Notice>}
        </Drawer>
      )}
    </Page>
  );
}

function TaskPanels({
  taskId,
  q,
  customerId,
  runner,
  startTurn,
}: {
  taskId: string;
  q: ReturnType<typeof useAgentTask>;
  customerId: string;
  runner: ReturnType<typeof useRunner>;
  startTurn: (message: string, conversationId?: string) => Promise<void>;
}) {
  const { workspaceId } = useSession();
  const ws = useWorkspaceLink();
  const navigate = useNavigate();
  const [acknowledged, setAcknowledged] = useState(false);
  if (q.isLoading) return <Skeleton lines={2} />;
  if (q.isError) return <PageError error={q.error} />;
  const task = q.data as AgentTask | undefined;
  if (!task) return null;
  if (task.agent !== "agent-application" || task.kind !== "app") {
    return (
      <p className="error" role="alert">
        That is not an ApplicationAgent app task.
      </p>
    );
  }
  const reply = taskReply(task);
  const result = taskResult(task);
  const link = <AgentEventsLink customerId={customerId} taskId={taskId} />;
  const wrap = (children: ReactNode) => <div className="mb-5 space-y-2">{children}</div>;

  if (taskWorking(task)) {
    return wrap(
      <Card tone="active" label="ApplicationAgent" title="ApplicationAgent Is Working" actions={<Loader2 size={16} className="animate-spin text-action" aria-hidden />}>
        <div aria-live="polite">
          <p className="muted m-0">It keeps working if you leave; this page checks again every 2 seconds.</p>
          <p role="status" className="mb-0">
            {reply}
          </p>
          {link}
        </div>
      </Card>,
    );
  }

  if (result.kind === "proposal" && result.confirm === true) {
    const shown = result;
    const app = (shown.app && typeof shown.app === "object" ? shown.app : {}) as Row;
    const access = typeof app.auth === "string" ? app.auth : "";
    const publicApp = access === "none" || access === "public";
    const repository = (app.repository && typeof app.repository === "object" ? app.repository : {}) as Row;
    const files = Array.isArray(shown.files) ? (shown.files as Row[]) : [];
    const confirm = async () => {
      // The server form's checks, against a fresh read of the task: still a confirmable
      // proposal, unchanged since it was shown, and a public app acknowledged as public.
      const fresh = await fetchAgentTask(taskId, customerId, workspaceId);
      const proposal = taskResult(fresh);
      if (fresh.agent !== "agent-application" || fresh.kind !== "app" || fresh.status !== "succeeded" || proposal.kind !== "proposal" || proposal.confirm !== true) {
        throw new ApiError(400, "invalid_input", "That task has no ApplicationAgent proposal to confirm.");
      }
      if (canonicalJson(proposal) !== canonicalJson(shown)) {
        throw new ApiError(409, "conflict", "The ApplicationAgent proposal changed before confirmation.");
      }
      if (publicApp && !acknowledged) {
        throw new ApiError(400, "invalid_input", "This app will be public. Tick the box to confirm that anyone with its address can open it.");
      }
      const context: Row = { confirmed_proposal: proposal };
      if (publicApp) context.confirm_public = true;
      const turn = await act<Row>("agent_turn", {
        customer_id: customerId,
        workspace_id: workspaceId,
        agent: "agent-application",
        kind: "app",
        message: "Confirm and deploy the reviewed proposal.",
        // lake checks these against the stored proposal before the turn gets its write tools.
        proposal_task_id: taskId,
        proposal_sha256: task.proposal_sha256 ?? "",
        context,
      });
      navigate(ws("/ship/apps") + qs({ customer_id: customerId, task: String(turn.task_id ?? "") }));
    };
    return wrap(
      <div id="proposal">
        <Card tone="active" label="ApplicationAgent proposal" title={`Proposed App: ${String(app.name ?? "")}`} footer={link}>
          <div className="ask-reply">
            <Markdown>{reply}</Markdown>
          </div>
          <KeyValue
            className="my-3"
            rows={[
              ["Mode", String(app.mode ?? "")],
              ["Framework", String(app.framework ?? "")],
              ["Access", publicApp ? "Public — no sign-in" : access === "" || access === "lake" ? "Lake sign-in" : access],
              ["Repository", String(repository.id ?? repository.name ?? "")],
              ["Path", String(app.path ?? "")],
            ]}
          />
          {files.map((file, index) => (
            <CopyBlock key={index} label={<code>{String(file.path ?? "")}</code>} value={String(file.contents ?? "")} />
          ))}
          <form
            className="stack mt-3"
            onSubmit={(event) => {
              event.preventDefault();
              void runner.run("confirm", confirm);
            }}
          >
            {publicApp && (
              <>
                <Banner tone="warn">This app will be public: anyone with its address can open it without signing in to lake.</Banner>
                <label>
                  <input type="checkbox" name="confirm_public" value="1" required checked={acknowledged} onChange={(e) => setAcknowledged(e.target.checked)} /> I understand this app will be
                  public
                </label>
              </>
            )}
            <div>
              <Button variant="primary" type="submit" loading={runner.busy === "confirm"}>
                <Rocket size={14} aria-hidden /> Confirm and deploy
              </Button>
            </div>
          </form>
        </Card>
      </div>,
    );
  }

  const app = (result.app && typeof result.app === "object" ? result.app : {}) as Row;
  const rollout = (app.rollout && typeof app.rollout === "object" ? app.rollout : {}) as Row;
  const url = (typeof app.url === "string" && app.url) || (typeof rollout.url === "string" && rollout.url) || (typeof app.host === "string" && app.host ? `https://${app.host}` : "");
  return wrap(
    <Card
      label="ApplicationAgent"
      title="ApplicationAgent Result"
      actions={
        url ? (
          <Button href={url}>
            <ExternalLink size={14} aria-hidden /> Open the app
          </Button>
        ) : undefined
      }
      footer={link}
    >
      <div className="ask-reply">
        <Markdown>{reply}</Markdown>
      </div>
      {needsReply(task, "agent-application") && (
        <form
          className="stack mt-3"
          onSubmit={(event) => {
            event.preventDefault();
            const form = formValues(event);
            void runner.run("reply", async () => {
              // The reply form's precondition: the task still asks something, in a conversation.
              const fresh = await fetchAgentTask(taskId, customerId, workspaceId);
              if (fresh.agent !== "agent-application" || fresh.kind !== "app" || !needsReply(fresh, "agent-application") || !fresh.conversation_id) {
                throw new ApiError(400, "invalid_input", "That task has no ApplicationAgent question to answer.");
              }
              await startTurn(form.reply ?? "", fresh.conversation_id);
            });
          }}
        >
          <label>
            Reply
            <textarea name="reply" rows={4} required />
          </label>
          <div>
            <Button variant="primary" type="submit" loading={runner.busy === "reply"}>
              Reply to ApplicationAgent
            </Button>
          </div>
        </form>
      )}
    </Card>,
  );
}
