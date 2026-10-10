/** What every Train, Ship and Ask page shares: the space and workspace it works in, the
 * `workspace_query` / `workspace_act` calls into the workspace Zetta, the page frame with lake's
 * workspace gate, and a Markdown renderer.
 *
 * `workspace_query` is an action (POST) although it only reads. Calling it through `act` would
 * refresh every read on screen after each answer, which refetches it again, forever; reads over
 * POST go through `postRead`, which answers like `read` and refreshes nothing. */
import { type ReactNode, useState } from "react";
import { useQuery, type UseQueryOptions } from "@tanstack/react-query";
import { Link } from "react-router-dom";
import ReactMarkdown from "react-markdown";
import remarkGfm from "remark-gfm";
import { act, useRead, type Row } from "@/lib/api";
import { postRead as libPostRead } from "@/lib/api";
import { useSession } from "@/lib/session";
import { Button, ErrorBlock, Page, Skeleton } from "@/lib/ui";

/* ------------------------------------------------------------------ calls */

/** A read capability that must be POSTed (`workspace_query`, `get_agent_task` when posted): the
 * same envelope and errors as `read`, and no refresh of the reads on screen. */
/** Lake's POSTed read (`@/lib/api` postRead), with the signal as the third argument. */
export function postRead<T = Row>(name: string, input: Row, signal?: AbortSignal): Promise<T> {
  return libPostRead<T>(name, input, { signal });
}

/** The space (customer) and workspace a workspace page works in. */
export function useWorkspace() {
  const { space, workspaceId, viewer } = useSession();
  return { customerId: space?.id ?? viewer.customer_id ?? "", workspaceId: workspaceId ?? "", space };
}

/** An engine read in the workspace Zetta: `workspace_query {capability, input}`. */
export function wq<T = Row>(scope: { customerId: string; workspaceId: string }, capability: string, input: Row = {}, signal?: AbortSignal) {
  return postRead<T>("workspace_query", { customer_id: scope.customerId, workspace_id: scope.workspaceId, capability, input }, signal);
}

/** An engine action in the workspace Zetta: `workspace_act {capability, input}`. */
export function wa<T = Row>(scope: { customerId: string; workspaceId: string }, capability: string, input: Row = {}) {
  return act<T>("workspace_act", { customer_id: scope.customerId, workspace_id: scope.workspaceId, capability, input });
}

/** `wq` as a query, keyed by the workspace, the engine name and its input. */
export function useWq<T = Row>(capability: string | null, input: Row = {}, options?: Partial<UseQueryOptions<T>>) {
  const scope = useWorkspace();
  return useQuery<T>({
    queryKey: ["workspace_query", scope.customerId, scope.workspaceId, capability, input],
    queryFn: ({ signal }) => wq<T>(scope, capability!, input, signal),
    ...options,
    enabled: !!capability && !!scope.customerId && !!scope.workspaceId && (options?.enabled ?? true),
  });
}

/** A lake read that takes the space and workspace (`list_stores`, `list_agent_launches`, …). */
export function useSpaceRead<T = Row>(name: string | null, input: Row = {}, options?: Partial<UseQueryOptions<T>> & { workspace?: boolean }) {
  const scope = useWorkspace();
  const { workspace = true, ...rest } = options ?? {};
  const params = { customer_id: scope.customerId, ...(workspace ? { workspace_id: scope.workspaceId } : {}), ...input };
  return useRead<T>(name, params, { ...rest, enabled: !!scope.customerId && (!workspace || !!scope.workspaceId) && (rest.enabled ?? true) });
}

/** A workspace page's address under the current workspace (lake prefixed every workspace link
 * on a `/w/<id>/` page the same way). */
export function useWsPath() {
  const { workspaceId } = useSession();
  return (path: string) => (workspaceId ? `/w/${encodeURIComponent(workspaceId)}${path}` : path);
}

/** A query string from values, empty ones left out: `qs({id, q})` → "id=…&q=…". */
export function qs(values: Record<string, string | number | null | undefined>): string {
  return Object.entries(values)
    .filter(([, v]) => v !== undefined && v !== null && v !== "")
    .map(([k, v]) => `${encodeURIComponent(k)}=${encodeURIComponent(String(v))}`)
    .join("&");
}

/* ------------------------------------------------------------------ the workspace frame */

export type WorkspaceState = { requested?: boolean; phase?: string; message?: string; ready?: boolean; workspace_id?: string };

/** `workspace_status` for the page's workspace; checks again every 15 seconds while the workspace
 * is being prepared (the gate's meta refresh). */
export function useWorkspaceState() {
  return useSpaceRead<WorkspaceState>("workspace_status", {}, {
    refetchInterval: (query) => {
      const state = query.state.data as WorkspaceState | undefined;
      return state && state.requested && !state.ready && state.phase !== "Failed" ? 15_000 : false;
    },
  });
}

/** Lake's workspace gate (web_ui.cpp workspace_gate): the workspace failed, is being prepared, or
 * has not been asked for yet. */
export function WorkspaceGate({ state }: { state: WorkspaceState }) {
  const { customerId } = useWorkspace();
  const { refresh } = useSession();
  if (state.requested) {
    if (state.phase === "Failed") {
      const message = state.message ?? "";
      const quota = message.startsWith("workspace quota reached");
      return (
        <>
          <div className="panel notice status-danger">
            <h2>Your Workspace Could Not Be Prepared</h2>
            <p>
              {quota
                ? `This lake already runs as many workspaces as it allows (${message}). An operator can raise the limit; yours starts as soon as there is room, or cancel the request below.`
                : message}
            </p>
          </div>
          <section className="panel">
            <h2>Cancel This Workspace</h2>
            <p>Withdraw the request so you can start again later.</p>
            <Link to={`/workspaces?${qs({ customer_id: customerId })}`}>Cancel workspace</Link>
          </section>
        </>
      );
    }
    return (
      <div className="panel" role="status">
        <h2>Preparing Your Workspace</h2>
        <p>
          lake asked for a Zetta cluster of your own, where your projects, data and models live apart from every other customer's.{" "}
          {state.phase ? `Status: ${state.phase}.` : "It has not been picked up yet."}
        </p>
        <p className="muted">This page checks again every 15 seconds.</p>
      </div>
    );
  }
  return <StartWorkspace customerId={customerId} onStarted={refresh} />;
}

function StartWorkspace({ customerId, onStarted }: { customerId: string; onStarted: () => void }) {
  const start = useAsync();
  return (
    <div className="panel">
      <h2>Start Your Workspace</h2>
      <p>
        Design, Train and Ship run on a Zetta cluster of your own, so your projects, data and models are never shared with another customer. It
        takes a minute or two to prepare.
      </p>
      {start.error ? <ErrorBlock error={start.error} /> : null}
      <Button
        variant="primary"
        loading={start.busy}
        onClick={() =>
          void start.run(async () => {
            await act("start_workspace", { customer_id: customerId });
            onStarted();
          })
        }
      >
        Start your workspace
      </Button>
    </div>
  );
}

/** A Design, Train or Ship page (web_ui_workspace): eyebrow, heading, sub, the error, then either
 * why it is unavailable, the workspace gate, or the content. `always` is shown ready or not (the
 * Data page's Storage, the Agents page's Agent Library). */
export function WorkspaceFrame({
  eyebrow,
  title,
  sub,
  children,
  before,
  always,
  actions,
}: {
  eyebrow: string;
  title: ReactNode;
  sub?: ReactNode;
  children: (state: WorkspaceState) => ReactNode;
  before?: ReactNode;
  always?: ReactNode;
  actions?: ReactNode;
}) {
  const { customerId } = useWorkspace();
  const status = useWorkspaceState();
  let body: ReactNode;
  if (!customerId) body = <div className="panel empty">You have no space to work in yet.</div>;
  else if (status.isLoading) body = <Skeleton />;
  else if (status.isError) body = <div className="panel empty">{(status.error as Error).message}</div>;
  else if (!status.data?.ready)
    body = (
      <>
        {before}
        <WorkspaceGate state={status.data ?? {}} />
      </>
    );
  else
    body = (
      <>
        {before}
        {children(status.data)}
      </>
    );
  return (
    <Page eyebrow={eyebrow} title={title} sub={sub} actions={actions}>
      {body}
      {customerId && !status.isError ? always : null}
    </Page>
  );
}

/* ------------------------------------------------------------------ helpers */

/** Run one async action at a time and keep its error, for buttons and forms. */
export function useAsync() {
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<unknown>(null);
  return {
    busy,
    error,
    setError,
    run: async (fn: () => Promise<unknown>) => {
      setBusy(true);
      setError(null);
      try {
        await fn();
        return true;
      } catch (e) {
        setError(e);
        return false;
      } finally {
        setBusy(false);
      }
    },
  };
}

/** Markdown rendered in the browser (an agent's reply, a proposal's description), never raw HTML. */
export function Markdown({ children }: { children: string }) {
  return (
    <div className="markdown-body">
      <ReactMarkdown remarkPlugins={[remarkGfm]}>{children}</ReactMarkdown>
    </div>
  );
}

/** A value as table text (web_ui.cpp cell_text): strings bare, numbers trimmed, the rest as JSON. */
export function cellText(value: unknown): string {
  if (value === null || value === undefined) return "";
  if (typeof value === "string") return value;
  if (typeof value === "number") return Number.isInteger(value) ? String(value) : String(Number(value.toPrecision(4)));
  return JSON.stringify(value);
}

/** The text of a field, as lake's json_text read it: strings as they are, numbers and booleans
 * printed, anything else empty. */
export function text(row: Row | null | undefined, key: string): string {
  const value = row?.[key];
  if (value === null || value === undefined) return "";
  if (typeof value === "string") return value;
  if (typeof value === "number" || typeof value === "boolean") return String(value);
  return "";
}

/** Seconds since a UTC timestamp, ISO-8601 or Postgres text ("2026-09-26 03:04:00.12+00";
 * fractions and offset ignored, as the server read it), or -1. */
export function secondsSince(timestamp: string, now = Date.now()): number {
  const m = /^(\d{4})-(\d{2})-(\d{2}).(\d{2}):(\d{2}):(\d{2})/.exec(timestamp);
  if (!m) return -1;
  const at = Date.UTC(Number(m[1]), Number(m[2]) - 1, Number(m[3]), Number(m[4]), Number(m[5]), Number(m[6]));
  return Math.max(0, Math.floor((now - at) / 1000));
}

/** "40 seconds", "2 minutes": a duration as a person says it. */
export function spokenDuration(seconds: number): string {
  if (seconds < 90) return `${seconds} seconds`;
  return `${Math.floor((seconds + 30) / 60)} minutes`;
}
