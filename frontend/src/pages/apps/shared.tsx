/** What the Ship, Experiments and Streams pages share: workspace links, permission flags, the
 * agent-task turn (Application Agent, Experiments Agent), Markdown, the platform pages' repository
 * tabs and stale banner, and small text helpers. Candidates to hoist into `lib/`. */
import { type ReactNode, useCallback, useState } from "react";
import { Link, useSearchParams } from "react-router-dom";
import { ApiError, read, useRead, type Row } from "@/lib/api";
import { useSession } from "@/lib/session";
import { useToast } from "@/lib/toast";
import { workspacePath } from "@/routes";
import { cx } from "@/lib/ui";
import "@/styles/ship.css";

/* ------------------------------------------------------------------ links */

/** A page address under the current workspace (`/w/<id>/…`). */
export function useWorkspaceLink(): (path: string) => string {
  const { workspaceId } = useSession();
  return useCallback((path: string) => workspacePath(workspaceId, path), [workspaceId]);
}

/** A query string from pairs, empty values left out. */
export function qs(params: Record<string, string | number | null | undefined>): string {
  const search = new URLSearchParams();
  for (const [key, value] of Object.entries(params)) {
    if (value !== undefined && value !== null && value !== "") search.set(key, String(value));
  }
  const text = search.toString();
  return text ? `?${text}` : "";
}

/** The Streams page for one topic, as lake links it from apps, builds and agent turns. */
export function streamHref(ws: (path: string) => string, customerId: string, topic: string, extra: Record<string, string> = {}): string {
  return ws("/train/stream") + qs({ customer_id: customerId, zetta: "workspace", topic, ...extra });
}

/** "View this turn in Streams" under an agent task (web_ui.cpp agent_events_link). */
export function AgentEventsLink({ customerId, taskId }: { customerId: string; taskId: string }) {
  const ws = useWorkspaceLink();
  if (!customerId || !taskId) return null;
  return (
    <p className="sub">
      <Link to={streamHref(ws, customerId, "agent-events", { task: taskId })}>View this turn in Streams</Link>
    </p>
  );
}

/* ------------------------------------------------------------------ permissions */

export type Permissions = { canWrite: boolean; canAdmin: boolean; loading: boolean };

/** Whether the viewer may write to, and administer, a space: `my_permissions` on the space's
 * policy object, as the server pages asked `control::authorize` (`can_write`, `can_admin`; promote
 * is the admin check). It only decides which buttons show; every capability checks again. */
export function useSpacePermissions(customerId: string | null | undefined): Permissions {
  const { viewer } = useSession();
  const q = useRead<{ allowed?: Record<string, boolean> }>(
    customerId ? "my_permissions" : null,
    { object_type: "customer", object_id: customerId ?? "", actions: JSON.stringify(["write", "admin"]) },
    { staleTime: 60_000 },
  );
  const allowed = q.data?.allowed ?? {};
  return {
    canWrite: viewer.administrator || !!allowed.write,
    canAdmin: viewer.administrator || !!allowed.admin,
    loading: q.isLoading,
  };
}

/* ------------------------------------------------------------------ agent turns */

export type AgentTask = {
  task_id: string;
  agent?: string;
  kind?: string;
  status?: string;
  message?: string;
  conversation_id?: string;
  answer?: Row | null;
  /** The digest of a proposal in the answer, as lake stored it; agent_turn's confirmation names it. */
  proposal_sha256?: string;
};

/** A task is working while it is pending or running; the page checks again every 2 seconds. */
export function taskWorking(task: AgentTask | undefined): boolean {
  return !!task && (task.status === "pending" || task.status === "running" || !task.status);
}

/** The task's `answer` (null on a failed task) as an object. */
export function taskAnswer(task: AgentTask | undefined): Row {
  const answer = task?.answer;
  return answer && typeof answer === "object" && !Array.isArray(answer) ? answer : {};
}

/** The structured result an agent answered (`answer.result`), as an object. */
export function taskResult(task: AgentTask | undefined): Row {
  const result = taskAnswer(task).result;
  return result && typeof result === "object" && !Array.isArray(result) ? result : {};
}

/** The reply shown for a task: the agent's `answer.reply`, else the task's message. */
export function taskReply(task: AgentTask | undefined): string {
  const reply = taskAnswer(task).reply;
  return typeof reply === "string" && reply ? reply : (task?.message ?? "");
}

/** Whether a finished task asks the person something (http_server.cpp agent_task_needs_reply). */
export function needsReply(task: AgentTask | undefined, agent: "agent-application" | "agent-experiments"): boolean {
  if (task?.status !== "succeeded") return false;
  const result = taskResult(task);
  const kind = typeof result.kind === "string" ? result.kind : "";
  if (kind === "question") return true;
  const subject = agent === "agent-application" ? result.app : result.study;
  const isObject = !!subject && typeof subject === "object" && !Array.isArray(subject);
  return kind === "" || (kind === "proposal" && (result.confirm !== true || !isObject));
}

/** JSON with object keys sorted, so two copies of one proposal compare equal however they were
 * built (the server page compared `sha256(proposal.dump())`, nlohmann's key-sorted dump). */
export function canonicalJson(value: unknown): string {
  if (Array.isArray(value)) return `[${value.map(canonicalJson).join(",")}]`;
  if (value && typeof value === "object") {
    const record = value as Record<string, unknown>;
    return `{${Object.keys(record)
      .sort()
      .map((key) => `${JSON.stringify(key)}:${canonicalJson(record[key])}`)
      .join(",")}}`;
  }
  return JSON.stringify(value ?? null);
}

/** One agent task, read again every 2 seconds while it works. */
export function useAgentTask(taskId: string, customerId: string, workspaceId: string | null) {
  return useRead<AgentTask>(
    taskId && customerId ? "get_agent_task" : null,
    { customer_id: customerId, workspace_id: workspaceId ?? undefined, task_id: taskId },
    { refetchInterval: (query) => (taskWorking(query.state.data as AgentTask | undefined) ? 2000 : false), staleTime: 0 },
  );
}

/** A fresh copy of a task, for the checks the confirm and reply forms made on the server. */
export function fetchAgentTask(taskId: string, customerId: string, workspaceId: string | null): Promise<AgentTask> {
  return read<AgentTask>("get_agent_task", { customer_id: customerId, workspace_id: workspaceId ?? undefined, task_id: taskId });
}

/** Whether a library agent is deployed on this lake (`list_agent_library`). */
export function useAgentDeployed(key: string): { deployed: boolean; loading: boolean } {
  const q = useRead<{ agents?: Row[] }>("list_agent_library", undefined, { staleTime: 60_000 });
  return { deployed: !!q.data?.agents?.some((agent) => agent.key === key && agent.deployed), loading: q.isLoading };
}

/* ------------------------------------------------------------------ markdown */

export { Markdown } from "@/lib/markdown";

/* ------------------------------------------------------------------ actions */

/** A page's write in progress and its error, shown where the server page showed its flash. */
export function useRunner() {
  const toast = useToast();
  const [error, setError] = useState<unknown>(null);
  const [busy, setBusy] = useState<string | null>(null);
  const run = useCallback(
    async <T,>(key: string, action: () => Promise<T>, done?: string | ((value: T) => string | undefined)): Promise<T | undefined> => {
      setBusy(key);
      setError(null);
      try {
        const value = await action();
        const text = typeof done === "function" ? done(value) : done;
        if (text) toast(text);
        return value;
      } catch (e) {
        setError(e);
        return undefined;
      } finally {
        setBusy(null);
      }
    },
    [toast],
  );
  return { error, setError, busy, run };
}

/** An action's error message, for an inline notice. */
export function errorText(error: unknown): string {
  if (!error) return "";
  if (error instanceof ApiError) return error.message;
  return error instanceof Error ? error.message : String(error);
}

/** The error a page shows at its top (the server page's `<p class="error" role="alert">`). */
export function PageError({ error }: { error: unknown }) {
  const text = errorText(error);
  if (!text) return null;
  return (
    <p className="error" role="alert">
      {text}
    </p>
  );
}

/* ------------------------------------------------------------------ platform */

/** "Platform data is stale." (web_ui.cpp platform_stale_banner). */
export function StaleBanner({ since }: { since?: string | null }) {
  if (!since) return null;
  return (
    <div className="mb-3 rounded-[5px] border-l-[3px] border-l-warn bg-warn-soft px-3.5 py-2.5 text-[13px] text-ink" role="status">
      <strong>Platform data is stale.</strong> The operator control plane has been unreachable since {since}. Writes are disabled.
    </div>
  );
}

/** A long identifier as its first 12 and last 7 characters (web_ui.cpp abbreviated_identifier). */
export function abbreviated(value: string | null | undefined): string {
  const text = value ?? "";
  return text.length <= 20 ? text : `${text.slice(0, 12)}…${text.slice(-7)}`;
}

/** An image reference as its digest's first 12 hex characters (web_ui.cpp short_image). */
export function shortImage(image: string | null | undefined): string {
  const text = image ?? "";
  const at = text.indexOf("sha256:");
  return at < 0 ? abbreviated(text) : text.slice(at, at + 19);
}

/** A URL as a link when it is http(s), else as text. */
export function UrlText({ url, empty = "—" }: { url?: string | null; empty?: ReactNode }) {
  if (!url) return <span className="muted">{empty}</span>;
  if (!/^https?:\/\//.test(url)) return <>{url}</>;
  return (
    <a href={url} rel="noreferrer">
      {url}
    </a>
  );
}

/** A platform row's field as text: absent and null read empty, other values as JSON. */
export function text(row: Row | null | undefined, key: string): string {
  const value = row?.[key];
  if (value === undefined || value === null) return "";
  return typeof value === "string" ? value : JSON.stringify(value);
}

export type Repository = { id: string; name?: string; customer_id?: string };

export function repositoryName(repository: Repository | undefined): string {
  return repository?.name || repository?.id || "";
}

/** A repository's tabs (web_ui.cpp forge_tabs): Files, Webhooks and Deployments need a signed-in
 * viewer; Protection needs the operator. */
export function RepositoryTabs({ repositoryId, current }: { repositoryId: string; current: string }) {
  const { viewer } = useSession();
  const id = encodeURIComponent(repositoryId);
  const tabs: [string, string, string][] = [
    ["Files", `/drive?repository_id=${id}`, "files"],
    ["Code", `/repository?id=${id}`, "code"],
    ["Changes", `/changes?repository_id=${id}`, "changes"],
    ["Issues", `/issues?repository_id=${id}`, "issues"],
    ["Releases", `/releases?repository_id=${id}`, "releases"],
    ["Artifacts", `/artifacts?repository_id=${id}`, "artifacts"],
    ["Deployments", `/environments?repository_id=${id}`, "platform"],
    ["Webhooks", `/webhooks?repository_id=${id}`, "webhooks"],
    ["Protection", `/protection?repository_id=${id}`, "protection"],
  ];
  return (
    <nav className="tabs forge-tabs" aria-label="Repository">
      {tabs
        .filter(([, , key]) => viewer.signed_in || !["files", "platform", "webhooks"].includes(key))
        .filter(([, , key]) => key !== "protection" || viewer.administrator)
        .map(([label, href, key]) => (
          <Link key={key} to={href} aria-current={key === current ? "page" : undefined}>
            {label}
          </Link>
        ))}
    </nav>
  );
}

/* ------------------------------------------------------------------ storage */

/** A remembered number (an event cursor), tolerating blocked storage. */
export function storedNumber(key: string): number {
  try {
    return Number(localStorage.getItem(key)) || 0;
  } catch {
    return 0;
  }
}

export function storeNumber(key: string, value: number) {
  try {
    localStorage.setItem(key, String(value));
  } catch {
    /* keep following with the in-memory cursor */
  }
}

/** Wait, unless the signal aborts first. */
export function pause(ms: number, signal?: AbortSignal): Promise<void> {
  return new Promise((resolve) => {
    const timer = setTimeout(resolve, ms);
    signal?.addEventListener(
      "abort",
      () => {
        clearTimeout(timer);
        resolve();
      },
      { once: true },
    );
  });
}

/* ------------------------------------------------------------------ page parts */

/** The tab a page shows, from `?tab=` (the first tab when absent or unknown). */
export function useQueryTab<T extends string>(ids: readonly T[]): T {
  const [params] = useSearchParams();
  const asked = params.get("tab") as T | null;
  return asked && ids.includes(asked) ? asked : ids[0];
}

/** Underlined tabs that are links to `?tab=<id>`, keeping every other query parameter (an app's
 * `app_id`, an experiment's `experiment`), so back and a shared address open the same tab. */
export function QueryTabs<T extends string>({ tabs, value, label = "Sections" }: { tabs: { id: T; label: ReactNode; count?: number }[]; value: T; label?: string }) {
  const [params] = useSearchParams();
  const href = (id: T, first: boolean) => {
    const next = new URLSearchParams(params);
    if (first) next.delete("tab");
    else next.set("tab", id);
    const text = next.toString();
    return text ? `?${text}` : "?";
  };
  return (
    <nav className="tabs mb-4" aria-label={label}>
      {tabs.map((tab, index) => (
        <Link key={tab.id} to={href(tab.id, index === 0)} replace aria-current={tab.id === value ? "page" : undefined}>
          {tab.label}
          {tab.count !== undefined && <span className="tab-count">{tab.count}</span>}
        </Link>
      ))}
    </nav>
  );
}

/** A dark terminal panel for live logs and events: a mono heading, actions on its right, and the
 * lines (or `empty` while there are none). */
export function Terminal({
  title,
  actions,
  children,
  empty,
  live,
  className,
}: {
  title?: ReactNode;
  actions?: ReactNode;
  children?: ReactNode;
  empty?: ReactNode;
  live?: boolean;
  className?: string;
}) {
  const blank = children === undefined || children === null || children === "" || (Array.isArray(children) && children.length === 0);
  return (
    <div className={cx("terminal", className)}>
      {(title || actions) && (
        <div className="terminal-head">
          {live !== undefined && <span className={cx("live-dot", live && "is-live")} aria-hidden />}
          <span>{title}</span>
          {actions && <div className="terminal-actions">{actions}</div>}
        </div>
      )}
      <div className="terminal-body" role="log" aria-live="polite">
        {blank ? <span className="terminal-empty">{empty ?? "Nothing yet."}</span> : children}
      </div>
    </div>
  );
}

/** The Live switch: a prominent toggle with a pulsing dot while it follows. */
export function LiveSwitch({ live, onChange, label = "Live", pausedLabel = "Paused" }: { live: boolean; onChange: (value: boolean) => void; label?: string; pausedLabel?: string }) {
  return (
    <button type="button" className="live-switch" aria-pressed={live} onClick={() => onChange(!live)} title={live ? "Pause live updates" : "Follow live updates"}>
      <span className="live-dot" aria-hidden />
      {live ? label : pausedLabel}
    </button>
  );
}
