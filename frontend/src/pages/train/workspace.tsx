/** What every Train, Ship and Ask page shares: the space and workspace it works in, the
 * `workspace_query` / `workspace_act` calls into the workspace Zetta, the page frame with lake's
 * workspace gate, and a Markdown renderer.
 *
 * `workspace_query` is an action (POST) although it only reads. Calling it through `act` would
 * refresh every read on screen after each answer, which refetches it again, forever; reads over
 * POST go through `postRead`, which answers like `read` and refreshes nothing. */
import { type ReactNode, useState } from "react";
import { useQuery, type UseQueryOptions } from "@tanstack/react-query";
import { act, useRead, type Row } from "@/lib/api";
import { pollUnlessFailed } from "@/lib/errors";
import { WorkspaceGate, useWorkspaceStatus, type WorkspaceState } from "@/components/WorkspaceGate";
import { postRead as libPostRead } from "@/lib/api";
import { useSession } from "@/lib/session";
import { Page } from "@/lib/ui";

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
    refetchInterval: pollUnlessFailed(options?.refetchInterval),
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

export type { WorkspaceState } from "@/components/WorkspaceGate";

/** `workspace_status` for the page's workspace (the shared gate's read). */
export function useWorkspaceState() {
  return useWorkspaceStatus().q;
}

/** Whether the page's workspace Zetta is ready: a header action that needs the workspace (New
 * agent, Load data) shows only then. */
export function useWorkspaceReady(): boolean {
  return !!useWorkspaceStatus().q.data?.ready;
}

/** A Design, Train or Ship page (web_ui_workspace): eyebrow, heading, sub, then the shared
 * workspace gate (components/WorkspaceGate.tsx) around the content. `always` is shown ready or not
 * (the Data page's Storage, the Agents page's Agent Library). */
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
  return (
    <Page eyebrow={eyebrow} title={title} sub={sub} actions={actions}>
      <WorkspaceGate before={before} after={always}>
        {(state) => children(state)}
      </WorkspaceGate>
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

export { Markdown } from "@/lib/markdown";

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

/** "5m ago", "3h ago", "2d ago" for a UTC timestamp (ISO-8601 or Postgres text, as
 * `secondsSince` reads it); an older one as its date, one it cannot read as it is. */
export function timeAgo(timestamp: string, now = Date.now()): string {
  if (!timestamp) return "—";
  const s = secondsSince(timestamp, now);
  if (s < 0) return timestamp;
  if (s < 60) return "just now";
  if (s < 3600) return `${Math.floor(s / 60)}m ago`;
  if (s < 86400) return `${Math.floor(s / 3600)}h ago`;
  if (s < 86400 * 14) return `${Math.floor(s / 86400)}d ago`;
  return timestamp.slice(0, 10);
}

/** "40 seconds", "2 minutes": a duration as a person says it. */
export function spokenDuration(seconds: number): string {
  if (seconds < 90) return `${seconds} seconds`;
  return `${Math.floor((seconds + 30) / 60)} minutes`;
}
