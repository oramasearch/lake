/** The calls every Design page shares: the space and workspace it works in, the engine reads and
 * actions through `workspace_query` / `workspace_act`, and links under the current workspace.
 *
 * `workspace_query` (and `delete_workspace {dry_run: true}`) are actions (POST) although they only
 * read. Calling them through `act` would refresh every read on screen after each answer, which
 * calls them again, forever; reads over POST go through lib's `postRead`, which refreshes nothing. */
import { useQuery, type UseQueryOptions } from "@tanstack/react-query";
import { act, postRead, useRead, type Params, type Row } from "@/lib/api";
import { pollUnlessFailed } from "@/lib/errors";
import { useSession } from "@/lib/session";

export { postRead };

export type Scope = { customerId: string; workspaceId: string };

/** The space (customer) and workspace a workspace page works in. */
export function useScope(): Scope {
  const { space, workspaceId, viewer } = useSession();
  return { customerId: space?.id ?? viewer.customer_id ?? "", workspaceId: workspaceId ?? "" };
}

/** An engine read in the workspace Zetta: `workspace_query {capability, input}`. */
export function wq<T = Row>(scope: Scope, capability: string, input: Row = {}, signal?: AbortSignal) {
  return postRead<T>("workspace_query", { customer_id: scope.customerId, workspace_id: scope.workspaceId, capability, input }, { signal });
}

/** An engine action in the workspace Zetta: `workspace_act {capability, input}`. */
export function wa<T = Row>(scope: Scope, capability: string, input: Row = {}) {
  return act<T>("workspace_act", { customer_id: scope.customerId, workspace_id: scope.workspaceId, capability, input });
}

/** `wq` as a query, keyed by the workspace, the engine capability and its input. */
export function useWq<T = Row>(capability: string | null, input: Row = {}, options?: Partial<UseQueryOptions<T>>) {
  const scope = useScope();
  return useQuery<T>({
    queryKey: ["workspace_query", scope.customerId, scope.workspaceId, capability, input],
    queryFn: ({ signal }) => wq<T>(scope, capability!, input, signal),
    ...options,
    refetchInterval: pollUnlessFailed(options?.refetchInterval),
    enabled: !!capability && !!scope.customerId && !!scope.workspaceId && (options?.enabled ?? true),
  });
}

/** A lake read that names the space and the workspace (`list_runs`, `get_agent_task`, …). */
export function useScopedRead<T = Row>(name: string | null, input: Params = {}, options?: Partial<UseQueryOptions<T>>) {
  const scope = useScope();
  return useRead<T>(name, { customer_id: scope.customerId, workspace_id: scope.workspaceId, ...input }, {
    ...options,
    // A null name reads nothing: useRead spreads these options over its own `enabled`, so the
    // name is checked here too (else it would call `/v1/null`).
    enabled: !!name && !!scope.customerId && !!scope.workspaceId && (options?.enabled ?? true),
  });
}

/** A workspace page's address under the current workspace: lake prefixed every workspace link on a
 * `/w/<id>/` page the same way. */
export function useWsPath(): (path: string) => string {
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
