// SPDX-License-Identifier: MIT

/** When an app fails (lake#1207, docs/ui/apps.md#when-an-app-fails): the notifications Lake keeps
 * in the workspace (`list_notifications`, general over the resource kind), *Fix with the
 * Application Agent* (`repair_app`), *Change This App* (`change_app`, a conversation since lake#1240:
 * AppConversation.tsx),
 * the opt-in auto-fix (`get_auto_fix`, `set_auto_fix`) and the review of a repair or change: the
 * diagnosis and the diff, confirmed with an `agent_turn` of the same kind. */
import { type ReactNode, useState } from "react";
import { Link, useNavigate } from "react-router-dom";
import { AlertTriangle, Bell, Loader2, Wand2, Wrench, X } from "lucide-react";
import { act, ApiError, useRead, type Row } from "@/lib/api";
import { relative } from "@/lib/format";
import { Banner, Button, Card, Empty, ErrorBlock, KeyValue, Notice, Skeleton, Status } from "@/lib/ui";
import { AppConversation } from "./AppConversation";
import {
  AgentEventsLink,
  type AgentTask,
  canonicalJson,
  fetchAgentTask,
  Markdown,
  PageError,
  qs,
  taskReply,
  taskResult,
  taskWorking,
  Terminal,
  useAgentTask,
  useRunner,
  useWorkspaceLink,
} from "./shared";

export type ResourceNotification = Row & {
  id: string;
  resource_kind: string;
  resource_id: string;
  resource_name?: string;
  event: string;
  reason: string;
  detail?: string;
  created_at?: string;
  for_you?: boolean;
  resolved?: boolean;
  dismissed?: boolean;
};

/** The headline of a notification, in words. */
export function notificationTitle(notification: ResourceNotification): string {
  const name = notification.resource_name || notification.resource_id;
  switch (notification.event) {
    case "build_failed":
      return `${name}: its image did not build`;
    case "auto_fix_stopped":
      return `${name}: auto-fix stopped`;
    case "app_errors":
      return `${name}: its code keeps failing`;
    default:
      return `${name} is not running`;
  }
}

/** The workspace's open notifications, newest first; `filter` narrows them to a kind or one resource. */
export function useNotifications(customerId: string, workspaceId: string | null, filter: { resource_kind?: string; resource_id?: string; include_resolved?: boolean } = {}) {
  return useRead<{ notifications?: ResourceNotification[]; open?: number }>(customerId && workspaceId ? "list_notifications" : null, {
    customer_id: customerId,
    workspace_id: workspaceId ?? undefined,
    ...filter,
  });
}

/** Where a notification's resource is, and its repair page. */
export function notificationHref(ws: (path: string) => string, customerId: string, notification: ResourceNotification, task?: string): string {
  if (notification.resource_kind === "app") {
    return ws("/ship/app") + qs({ customer_id: customerId, app_id: notification.resource_id, task: task ?? null });
  }
  return ws("/ship/apps") + qs({ customer_id: customerId });
}

/** Starts a repair turn for an app and opens its page on the turn. */
function useStartRepair(customerId: string, workspaceId: string | null) {
  const navigate = useNavigate();
  const ws = useWorkspaceLink();
  return async (appId: string, notificationId?: string) => {
    const input: Row = { customer_id: customerId, workspace_id: workspaceId, app_id: appId };
    if (notificationId) input.notification_id = notificationId;
    const started = await act<Row>("repair_app", input);
    navigate(ws("/ship/app") + qs({ customer_id: customerId, app_id: appId, task: String(started.task_id ?? "") }));
  };
}

/** One notification as a banner: the reason, Fix with the Application Agent, Open and Dismiss. */
export function NotificationBanner({
  notification,
  customerId,
  workspaceId,
  runner,
  onDismissed,
  showOpen,
}: {
  notification: ResourceNotification;
  customerId: string;
  workspaceId: string | null;
  runner: ReturnType<typeof useRunner>;
  onDismissed: () => void;
  showOpen?: boolean;
}) {
  const ws = useWorkspaceLink();
  const repair = useStartRepair(customerId, workspaceId);
  const fixKey = `fix-${notification.id}`;
  const dismissKey = `dismiss-${notification.id}`;
  const fixable = notification.resource_kind === "app" && !notification.resolved;
  return (
    <Banner
      tone={notification.resolved ? "quiet" : "warn"}
      title={
        <span className="inline-flex flex-wrap items-center gap-2">
          <AlertTriangle size={14} aria-hidden /> {notificationTitle(notification)}
          {notification.for_you ? <Status value="for you" tone="info" /> : null}
          {notification.resolved ? <Status value="resolved" tone="success" /> : null}
          <span className="text-[12px] font-normal text-ink-3">{relative(notification.created_at)}</span>
        </span>
      }
      action={
        <span className="inline-flex flex-wrap gap-2">
          {fixable && (
            <Button variant="primary" loading={runner.busy === fixKey} onClick={() => void runner.run(fixKey, () => repair(notification.resource_id, notification.id))}>
              <Wrench size={14} aria-hidden /> Fix with the Application Agent
            </Button>
          )}
          {showOpen && <Button to={notificationHref(ws, customerId, notification)}>Open</Button>}
          <Button
            variant="ghost"
            aria-label="Dismiss"
            loading={runner.busy === dismissKey}
            onClick={() =>
              void runner
                .run(dismissKey, () => act("dismiss_notification", { customer_id: customerId, workspace_id: workspaceId, notification_id: notification.id }))
                .then((done) => {
                  if (done !== undefined) onDismissed();
                })
            }
          >
            <X size={14} aria-hidden />
          </Button>
        </span>
      }
    >
      {notification.reason}
      {notification.detail ? <span className="mt-1 block font-mono text-[12px] text-ink-3">{notification.detail}</span> : null}
    </Banner>
  );
}

/** Ship → Apps: the open notifications about the workspace's apps, above the grid. */
export function AppNotifications({ customerId, workspaceId }: { customerId: string; workspaceId: string | null }) {
  const runner = useRunner();
  const notifications = useNotifications(customerId, workspaceId, { resource_kind: "app" });
  const items = notifications.data?.notifications ?? [];
  if (notifications.isError) return <ErrorBlock error={notifications.error} retry={() => void notifications.refetch()} what="app notifications" />;
  if (items.length === 0) return null;
  return (
    <div className="mb-5 space-y-2" aria-label="App notifications">
      <PageError error={runner.error} />
      {items.map((notification) => (
        <NotificationBanner key={notification.id} notification={notification} customerId={customerId} workspaceId={workspaceId} runner={runner} onDismissed={() => void notifications.refetch()} showOpen />
      ))}
    </div>
  );
}

/** Account → Notifications: every notification of the current workspace, resolved ones included. */
export function NotificationsList({ customerId, workspaceId }: { customerId: string; workspaceId: string | null }) {
  const runner = useRunner();
  const notifications = useNotifications(customerId, workspaceId, { include_resolved: true });
  if (!customerId || !workspaceId) {
    return (
      <Empty icon={Bell} title="Notifications">
        Choose a workspace to see what Lake has told you about it.
      </Empty>
    );
  }
  if (notifications.isLoading) return <Skeleton lines={2} />;
  if (notifications.isError) return <ErrorBlock error={notifications.error} retry={() => void notifications.refetch()} what="notifications" />;
  const items = notifications.data?.notifications ?? [];
  if (items.length === 0) {
    return (
      <Empty icon={Bell} title="No notifications">
        Lake tells you here when an app you deployed fails, its image does not build, or auto-fix stops.
      </Empty>
    );
  }
  return (
    <div className="space-y-2">
      <PageError error={runner.error} />
      {items.map((notification) => (
        <NotificationBanner key={notification.id} notification={notification} customerId={customerId} workspaceId={workspaceId} runner={runner} onDismissed={() => void notifications.refetch()} showOpen />
      ))}
    </div>
  );
}

/** The app page's side card: its open notifications with Fix, auto-fix, and Change this app. */
export function AppRepairCard({ app, customerId, workspaceId, agentDeployed }: { app: Row; customerId: string; workspaceId: string | null; agentDeployed: boolean }) {
  const runner = useRunner();
  const appId = String(app.id ?? "");
  const notifications = useNotifications(customerId, workspaceId, { resource_kind: "app", resource_id: appId });
  const settings = useRead<Row>(customerId && appId ? "get_auto_fix" : null, { customer_id: customerId, workspace_id: workspaceId ?? undefined, resource_kind: "app", resource_id: appId });
  const repair = useStartRepair(customerId, workspaceId);
  const open = notifications.data?.notifications ?? [];
  const autoFix = settings.data?.auto_fix === true;
  const [attempts, setAttempts] = useState("3");

  return (
    <Card label="Application Agent" title="Fix or Change This App">
      <PageError error={runner.error} />
      {notifications.isError ? <ErrorBlock error={notifications.error} retry={() => void notifications.refetch()} what="this app's notifications" /> : null}
      {open.map((notification) => (
        <div key={notification.id} className="mb-3">
          <NotificationBanner notification={notification} customerId={customerId} workspaceId={workspaceId} runner={runner} onDismissed={() => void notifications.refetch()} />
        </div>
      ))}
      {open.length === 0 && (
        <div className="mb-3">
          <Button disabled={!agentDeployed} loading={runner.busy === "fix"} onClick={() => void runner.run("fix", () => repair(appId))}>
            <Wrench size={14} aria-hidden /> Fix with the Application Agent
          </Button>
        </div>
      )}
      <div className="border-t border-hairline pt-3">
        <h4 className="mb-2 mt-0 text-[13px]">Change This App</h4>
        <AppConversation appId={appId} customerId={customerId} workspaceId={workspaceId} agentDeployed={agentDeployed} className="max-h-[560px]" />
      </div>
      <div className="mt-3 border-t border-hairline pt-3">
        {settings.isError ? (
          <ErrorBlock error={settings.error} retry={() => void settings.refetch()} what="auto-fix" />
        ) : (
          <>
            <KeyValue
              rows={[
                ["Auto-fix", <Status key="a" value={autoFix ? (settings.data?.auto_fix_stopped ? "stopped" : "on") : "off"} tone={autoFix ? (settings.data?.auto_fix_stopped ? "warning" : "success") : "neutral"} />],
                ...(autoFix ? ([["Attempts", <span key="n" className="font-mono">{`${String(settings.data?.attempts ?? 0)} of ${String(settings.data?.max_attempts ?? 3)}`}</span>]] as [string, ReactNode][]) : []),
              ]}
            />
            {settings.data?.stopped_reason ? <p className="muted mb-2 text-[12.5px]">{String(settings.data.stopped_reason)}</p> : null}
            <p className="muted my-2 text-[12.5px]">
              With auto-fix on, the Application Agent commits a fix and redeploys by itself when the rollout fails, acting as you, up to the number of attempts you choose. Each attempt is a commit you can see and revert.
            </p>
            {autoFix ? (
              <Button loading={runner.busy === "auto"} onClick={() => void runner.run("auto", () => act("set_auto_fix", { customer_id: customerId, workspace_id: workspaceId, resource_kind: "app", resource_id: appId, enabled: false }), "Auto-fix turned off").then(() => void settings.refetch())}>
                Turn auto-fix off
              </Button>
            ) : (
              <div className="row-form flex flex-wrap items-end gap-2">
                <label className="w-28">
                  Attempts
                  <input type="number" min={1} max={10} value={attempts} onChange={(event) => setAttempts(event.target.value)} />
                </label>
                <Button
                  disabled={!agentDeployed}
                  loading={runner.busy === "auto"}
                  onClick={() =>
                    void runner
                      .run(
                        "auto",
                        () => act("set_auto_fix", { customer_id: customerId, workspace_id: workspaceId, resource_kind: "app", resource_id: appId, enabled: true, max_attempts: Math.min(10, Math.max(1, Number(attempts) || 3)) }),
                        "Auto-fix turned on",
                      )
                      .then(() => void settings.refetch())
                  }
                >
                  <Wand2 size={14} aria-hidden /> Turn auto-fix on
                </Button>
              </div>
            )}
          </>
        )}
      </div>
      {!agentDeployed && <Notice tone="warning">The Application Agent is not running on this Lake.</Notice>}
    </Card>
  );
}

/** A repair or change turn on the app page (`?task=`): working, the proposed diff to confirm, or
 * the result, in the same conversation. */
export function AppTurnPanel({ taskId, appId, customerId, workspaceId }: { taskId: string; appId: string; customerId: string; workspaceId: string | null }) {
  const q = useAgentTask(taskId, customerId, workspaceId);
  const runner = useRunner();
  const navigate = useNavigate();
  const ws = useWorkspaceLink();
  if (q.isLoading) return <Skeleton lines={2} />;
  if (q.isError) return <PageError error={q.error} />;
  const task = q.data as AgentTask | undefined;
  if (!task) return null;
  const kind = task.kind === "change" ? "change" : "repair";
  if (task.agent !== "agent-application" || (task.kind !== "repair" && task.kind !== "change")) {
    return (
      <p className="error" role="alert">
        That is not an Application Agent repair or change of this app.
      </p>
    );
  }
  const reply = taskReply(task);
  const result = taskResult(task);
  const link = <AgentEventsLink customerId={customerId} taskId={taskId} />;
  const title = kind === "change" ? "Change" : "Repair";
  const open = (next: string) => navigate(ws("/ship/app") + qs({ customer_id: customerId, app_id: appId, task: next }));

  if (taskWorking(task)) {
    return (
      <Card tone="active" label="Application Agent" title={`${title} in Progress`} actions={<Loader2 size={16} className="animate-spin text-action" aria-hidden />}>
        <div aria-live="polite">
          <p className="muted m-0">It keeps working if you leave; this page checks again every 2 seconds.</p>
          <p role="status" className="mb-0">
            {reply}
          </p>
          {link}
        </div>
      </Card>
    );
  }

  const followUp = (
    <form
      className="stack mt-3"
      onSubmit={(event) => {
        event.preventDefault();
        const element = event.currentTarget;
        const message = String(new FormData(element).get("message") ?? "").trim();
        if (!message) return;
        void runner.run("follow", async () => {
          const started = await act<Row>(kind === "change" ? "change_app" : "repair_app", {
            customer_id: customerId,
            workspace_id: workspaceId,
            app_id: appId,
            message,
            conversation_id: task.conversation_id,
          });
          element.reset();
          open(String(started.task_id ?? ""));
        });
      }}
    >
      <label>
        {kind === "change" ? "More instructions" : "Tell the agent more"}
        <textarea name="message" rows={3} />
      </label>
      <div>
        <Button type="submit" loading={runner.busy === "follow"}>
          Send to the Application Agent
        </Button>
      </div>
    </form>
  );

  if (result.kind === "proposal" && result.confirm === true) {
    const shown = result;
    const confirm = async () => {
      // Lake checks the same again: the stored proposal, unchanged since it was shown, of this app.
      const fresh = await fetchAgentTask(taskId, customerId, workspaceId);
      const proposal = taskResult(fresh);
      if (fresh.status !== "succeeded" || proposal.kind !== "proposal" || proposal.confirm !== true) {
        throw new ApiError(400, "invalid_input", "That task has no Application Agent proposal to confirm.");
      }
      if (canonicalJson(proposal) !== canonicalJson(shown)) {
        throw new ApiError(409, "conflict", "The Application Agent proposal changed before confirmation.");
      }
      const turn = await act<Row>("agent_turn", {
        customer_id: customerId,
        workspace_id: workspaceId,
        agent: "agent-application",
        kind,
        message: kind === "change" ? "Apply the reviewed change." : "Apply the reviewed fix.",
        conversation_id: fresh.conversation_id,
        proposal_task_id: taskId,
        proposal_sha256: fresh.proposal_sha256 ?? "",
        context: { app_id: appId, confirmed_proposal: proposal },
      });
      open(String(turn.task_id ?? ""));
    };
    const files = Array.isArray(shown.files) ? (shown.files as Row[]) : [];
    return (
      <div id="proposal">
        <Card tone="active" label="Application Agent proposal" title={kind === "change" ? "Proposed Change" : "Proposed Fix"} footer={link}>
          <PageError error={runner.error} />
          <div className="ask-reply">
            <Markdown>{reply}</Markdown>
          </div>
          <p className="muted mb-1 mt-3 text-[12.5px]">
            {files.length} file{files.length === 1 ? "" : "s"} change. Confirmed, the agent commits them to the app's repository and redeploys.
          </p>
          <Terminal title="Diff" empty="No diff.">
            {String(shown.diff ?? "")}
          </Terminal>
          <div className="mt-3">
            <Button variant="primary" loading={runner.busy === "confirm"} onClick={() => void runner.run("confirm", confirm)}>
              <Wrench size={14} aria-hidden /> {kind === "change" ? "Confirm and apply the change" : "Confirm and apply the fix"}
            </Button>
          </div>
          {followUp}
        </Card>
      </div>
    );
  }

  return (
    <Card label="Application Agent" title={`${title} Result`} footer={link}>
      <PageError error={runner.error} />
      <div className="ask-reply">
        <Markdown>{reply || "The Application Agent did not answer."}</Markdown>
      </div>
      {typeof result.diff === "string" && result.diff ? (
        <details className="mt-2">
          <summary className="cursor-pointer text-[13px] text-ink-2">What changed</summary>
          <Terminal className="mt-2" title="Diff">
            {result.diff}
          </Terminal>
        </details>
      ) : null}
      {followUp}
      <p className="muted mb-0 mt-2 text-[12.5px]">
        <Link to={ws("/ship/app") + qs({ customer_id: customerId, app_id: appId })}>Close</Link>
      </p>
    </Card>
  );
}
