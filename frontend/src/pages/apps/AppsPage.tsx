/** Ship → Apps (web_ui.cpp web_ui_apps; handlers serve_apps_page and serve_app_form create,
 * agent and agent_confirm): the space's apps, the two create forms, and an ApplicationAgent turn
 * with its proposal confirmed against a fresh read of the task. */
import { type FormEvent, useState } from "react";
import { Link, useNavigate, useSearchParams } from "react-router-dom";
import { act, ApiError, useList, type Row } from "@/lib/api";
import { useSession } from "@/lib/session";
import { DataTable, Notice, Page, Skeleton, Badge, type Column } from "@/lib/ui";
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

/** An app's phase as lake's badge: ready is success, Failed danger, Pending warning. */
export function AppPhase({ phase, ready }: { phase?: string | null; ready?: boolean }) {
  const words = phase || "Pending";
  return <Badge tone={ready ? "success" : words === "Failed" ? "danger" : words === "Pending" ? "warning" : "neutral"}>{words}</Badge>;
}

/** An app's address: a link when it is http(s), else its text, else "Not assigned". */
export function AppUrl({ url }: { url?: string | null }) {
  if (!url) return <span className="muted">Not assigned</span>;
  if (!/^https?:\/\//.test(url)) return <>{url}</>;
  return (
    <a href={url} rel="noreferrer">
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

export default function AppsPage() {
  const { space, workspaceId } = useSession();
  const ws = useWorkspaceLink();
  const navigate = useNavigate();
  const [params] = useSearchParams();
  const customerId = space?.id ?? "";
  const taskId = params.get("task") ?? "";
  const flash = params.get("error") ?? "";
  const runner = useRunner();

  const apps = useList<Row>(customerId ? "list_apps" : null, { customer_id: customerId, workspace_id: workspaceId ?? undefined }, { pick: "apps" });
  const repositories = useList<Row>(customerId ? "list_repositories" : null, { customer_id: customerId, limit: 500 }, { pick: "items", workspace: null });
  const agent = useAgentDeployed("agent-application");
  const task = useAgentTask(taskId, customerId, workspaceId);

  if (!space) {
    return (
      <Page eyebrow="Ship" title="Apps" sub="Create, deploy and operate customer applications with live rollout status.">
        <div className="panel empty">You have no space to work in yet.</div>
      </Page>
    );
  }

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

  const columns: Column<Row>[] = [
    { key: "name", label: "Name", render: (app) => <Link to={ws("/ship/app") + qs({ customer_id: customerId, app_id: String(app.id) })}>{String(app.name ?? "")}</Link> },
    { key: "url", label: "URL", render: (app) => <AppUrl url={app.url} /> },
    { key: "status", label: "Status", render: (app) => <AppPhase phase={app.phase} ready={!!app.ready} /> },
    { key: "mode", label: "Mode", render: (app) => String(app.mode ?? "") },
    { key: "revision", label: "Revision", render: (app) => <code>{app.mode === "runtime" ? String(app.source?.revision ?? "") : String(app.image ?? "")}</code> },
    { key: "bindings", label: "Bindings", render: (app) => bindingsSummary(app) },
  ];

  return (
    <Page eyebrow="Ship" title="Apps" sub="Create, deploy and operate customer applications with live rollout status.">
      {flash && !runner.error && (
        <p className="error" role="alert">
          {flash}
        </p>
      )}
      <PageError error={runner.error} />
      <DataTable
        rows={apps.data}
        q={apps}
        what="apps"
        columns={columns}
        rowKey={(app) => String(app.id)}
        caption="Apps"
        empty={<div className="panel empty">No apps yet. Create one from a Lake repository or a container image.</div>}
      />
      <div className="grid">
        <section className="panel">
          <h2>Create From a Repository</h2>
          <form className="stack" onSubmit={create}>
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
            <button type="submit" disabled={runner.busy === "create"}>
              Create App
            </button>
          </form>
        </section>
        <section className="panel">
          <h2>Create From an Image</h2>
          <form className="stack" onSubmit={create}>
            <input type="hidden" name="mode" value="image" />
            <label>
              Name
              <input name="name" required pattern="[a-z][a-z0-9-]{0,30}" />
            </label>
            <label>
              Image
              <input name="image" placeholder="registry.example/app:tag" required />
            </label>
            <label>
              Framework
              <input name="framework" defaultValue="container" required />
            </label>
            <label>
              Port
              <input type="number" name="port" defaultValue="3000" min={1} max={65535} required />
            </label>
            <button type="submit" disabled={runner.busy === "create"}>
              Create App
            </button>
          </form>
        </section>
      </div>
      <section className="panel">
        <h2>Build an App With the ApplicationAgent</h2>
        <p className="sub">Describe the app. The agent proposes files and app settings for you to confirm.</p>
        <form
          className="stack"
          onSubmit={(event) => {
            event.preventDefault();
            const form = formValues(event);
            void runner.run("agent", () => startTurn(form.description ?? ""));
          }}
        >
          <label>
            Description
            <textarea name="description" placeholder="What should the app do?" disabled={!agent.deployed} />
          </label>
          <button type="submit" disabled={!agent.deployed || runner.busy === "agent"}>
            Start ApplicationAgent
          </button>
        </form>
        {!agent.deployed && !agent.loading && <Notice tone="warning">ApplicationAgent is not registered on this lake yet.</Notice>}
      </section>
      {taskId && <TaskPanels taskId={taskId} q={task} customerId={customerId} runner={runner} startTurn={startTurn} />}
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

  if (taskWorking(task)) {
    return (
      <>
        {link}
        <section className="panel" aria-live="polite">
          <h2>ApplicationAgent Is Working</h2>
          <p>It keeps working if you leave; this page checks again every 2 seconds.</p>
          <p role="status">{reply}</p>
        </section>
      </>
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
    return (
      <>
        {link}
        <section className="panel" id="proposal">
          <h2>Proposed App: {String(app.name ?? "")}</h2>
          <div className="ask-reply">
            <Markdown>{reply}</Markdown>
          </div>
          <dl>
            <dt>Mode</dt>
            <dd>{String(app.mode ?? "")}</dd>
            <dt>Framework</dt>
            <dd>{String(app.framework ?? "")}</dd>
            <dt>Access</dt>
            <dd>{publicApp ? "Public — no sign-in" : access === "" || access === "lake" ? "Lake sign-in" : access}</dd>
            <dt>Repository</dt>
            <dd>{String(repository.id ?? repository.name ?? "")}</dd>
            <dt>Path</dt>
            <dd>{String(app.path ?? "")}</dd>
          </dl>
          {files.map((file, index) => (
            <div key={index}>
              <h3>
                <code>{String(file.path ?? "")}</code>
              </h3>
              <pre>
                <code>{String(file.contents ?? "")}</code>
              </pre>
            </div>
          ))}
          <form
            className="stack"
            onSubmit={(event) => {
              event.preventDefault();
              void runner.run("confirm", confirm);
            }}
          >
            {publicApp && (
              <>
                <p className="notice status-warning" role="note">
                  This app will be public: anyone with its address can open it without signing in to lake.
                </p>
                <label>
                  <input type="checkbox" name="confirm_public" value="1" required checked={acknowledged} onChange={(e) => setAcknowledged(e.target.checked)} /> I understand this app will be
                  public
                </label>
              </>
            )}
            <button className="button button-primary" type="submit" disabled={runner.busy === "confirm"}>
              Confirm and deploy
            </button>
          </form>
        </section>
      </>
    );
  }

  const app = (result.app && typeof result.app === "object" ? result.app : {}) as Row;
  const rollout = (app.rollout && typeof app.rollout === "object" ? app.rollout : {}) as Row;
  const url = (typeof app.url === "string" && app.url) || (typeof rollout.url === "string" && rollout.url) || (typeof app.host === "string" && app.host ? `https://${app.host}` : "");
  return (
    <>
      {link}
      <section className="panel">
        <h2>ApplicationAgent Result</h2>
        <div className="ask-reply">
          <Markdown>{reply}</Markdown>
        </div>
        {url && (
          <p>
            <a href={url}>Open the app</a>
          </p>
        )}
        {needsReply(task, "agent-application") && (
          <>
            <h3>Reply</h3>
            <form
              className="stack"
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
              <button className="button button-primary" type="submit" disabled={runner.busy === "reply"}>
                Reply to ApplicationAgent
              </button>
            </form>
          </>
        )}
      </section>
    </>
  );
}
