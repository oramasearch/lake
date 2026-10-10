/** The API client: the app calls lake's `/v1` capabilities and nothing else, the same ones MCP
 * and agents call (docs/api/README.md). A read is `GET /v1/<name>?<input>`, an action is
 * `POST /v1/<name>` with the input as JSON. Both answer `{ok, value, duration_ms}` or
 * `{ok: false, error: {code, message, path}}` (docs/api/errors.md).
 *
 * The credential is the HttpOnly `lake_session` cookie that sign-in sets. The page never sees the
 * secret, and the cookie is resolved by the same `context_for` as a bearer, so the app can do
 * nothing the API cannot. */
import { QueryClient, useMutation, useQuery, type UseQueryOptions } from "@tanstack/react-query";

export class ApiError extends Error {
  constructor(
    public status: number,
    public code: string,
    message: string,
    /** JSON pointer to the input field at fault, when the input is at fault. */
    public path = "",
  ) {
    super(message);
  }
}

/** A 401 means the session ended (expired or revoked), never "no access". Every 401 reads the same
 * and tells the shell to offer "Sign in again". */
export const SESSION_ENDED = "Your session has ended. Sign in again.";
const sessionEndedListeners = new Set<() => void>();
export function onSessionEnded(listener: () => void): () => void {
  sessionEndedListeners.add(listener);
  return () => {
    sessionEndedListeners.delete(listener);
  };
}
/** Whether a 401 should announce the session's end: false while nobody is signed in yet. */
let announceSessionEnd = false;
export function setSignedIn(value: boolean) {
  announceSessionEnd = value;
}
/** Whether someone is signed in: a 401 then means their session ended, not a failed sign-in. */
export function isSignedIn(): boolean {
  return announceSessionEnd;
}

function failure(status: number, code: string, message: string, path = ""): ApiError {
  if (status === 401) {
    if (announceSessionEnd) sessionEndedListeners.forEach((listener) => listener());
    return new ApiError(401, code, announceSessionEnd ? SESSION_ENDED : message, path);
  }
  return new ApiError(status, code, message, path);
}

// eslint-disable-next-line @typescript-eslint/no-explicit-any
export type Row = Record<string, any>;
export type Params = Record<string, string | number | boolean | null | undefined | string[]>;

/** The query string for a read's input. Arrays repeat the key; empty values are left out. */
export function query(params?: Params): string {
  if (!params) return "";
  const parts: string[] = [];
  for (const [key, value] of Object.entries(params)) {
    if (value === undefined || value === null || value === "") continue;
    for (const one of Array.isArray(value) ? value : [value]) {
      parts.push(`${encodeURIComponent(key)}=${encodeURIComponent(String(one))}`);
    }
  }
  return parts.length ? `?${parts.join("&")}` : "";
}

/** The current workspace scope (`/w/<id>`), set by the router from the page URL. A capability
 * called inside a workspace page is called under the same prefix, which is how lake picks the
 * workspace (docs/ui/workspace-selection.md). */
let workspacePrefix = "";
export function setWorkspace(id: string | null) {
  workspacePrefix = id ? `/w/${encodeURIComponent(id)}` : "";
}
export function currentWorkspace(): string | null {
  return workspacePrefix ? decodeURIComponent(workspacePrefix.slice(3)) : null;
}

let refreshQueued = false;
/** Every successful write marks every cached read stale and refetches the ones on screen: a write
 * changes more than the collection it posts to, and naming each affected key at each call site is
 * how lists go stale. Writes in the same tick share one refresh. */
export function refreshAfterWrite() {
  if (refreshQueued) return;
  refreshQueued = true;
  queueMicrotask(() => {
    refreshQueued = false;
    void queryClient.invalidateQueries();
  });
}

type CallOptions = { workspace?: string | null; signal?: AbortSignal };

function prefix(options?: CallOptions): string {
  if (options && options.workspace !== undefined) {
    return options.workspace ? `/w/${encodeURIComponent(options.workspace)}` : "";
  }
  return workspacePrefix;
}

async function unwrap<T>(response: Response): Promise<T> {
  const body = await response.json().catch(() => null);
  if (!response.ok || !body || body.ok === false) {
    const error = body?.error ?? {};
    throw failure(response.status, error.code ?? "error", error.message ?? response.statusText ?? "The request failed.", error.path ?? "");
  }
  return body.value as T;
}

/** Call a read capability: `read("list_repositories", {customer_id})`. */
export async function read<T = Row>(name: string, input?: Params, options?: CallOptions): Promise<T> {
  let response: Response;
  try {
    response = await fetch(`${prefix(options)}/v1/${name}${query(input)}`, {
      headers: { Accept: "application/json" },
      credentials: "same-origin",
      signal: options?.signal,
    });
  } catch (error) {
    if ((error as Error).name === "AbortError") throw error;
    throw new ApiError(0, "network", "Lake could not be reached. Check your connection and retry.");
  }
  return unwrap<T>(response);
}

/** Call an action capability: `act("create_repository", {name, customer_id})`. */
export async function act<T = Row>(name: string, input: Row = {}, options?: CallOptions): Promise<T> {
  let response: Response;
  try {
    response = await fetch(`${prefix(options)}/v1/${name}`, {
      method: "POST",
      headers: { Accept: "application/json", "Content-Type": "application/json" },
      credentials: "same-origin",
      body: JSON.stringify(input),
      signal: options?.signal,
    });
  } catch (error) {
    if ((error as Error).name === "AbortError") throw error;
    throw new ApiError(0, "network", "Lake could not be reached. Check your connection and retry.");
  }
  const value = await unwrap<T>(response);
  refreshAfterWrite();
  return value;
}

/** Call a capability that reads but is POSTed (`workspace_query`, `workspace_sql` with a body):
 * like `act`, but it changes nothing, so it refreshes no other read. Inside a query, use this or
 * `usePostRead`, never `act`: `act` refreshes every read, including the one that called it. */
export async function postRead<T = Row>(name: string, input: Row = {}, options?: CallOptions): Promise<T> {
  let response: Response;
  try {
    response = await fetch(`${prefix(options)}/v1/${name}`, {
      method: "POST",
      headers: { Accept: "application/json", "Content-Type": "application/json" },
      credentials: "same-origin",
      body: JSON.stringify(input),
      signal: options?.signal,
    });
  } catch (error) {
    if ((error as Error).name === "AbortError") throw error;
    throw new ApiError(0, "network", "Lake could not be reached. Check your connection and retry.");
  }
  return unwrap<T>(response);
}

/** A non-capability endpoint that answers JSON (sign-in, `/session`): same error handling. */
export async function post<T = Row>(path: string, body: Row): Promise<T> {
  let response: Response;
  try {
    response = await fetch(path, {
      method: "POST",
      headers: { Accept: "application/json", "Content-Type": "application/json" },
      credentials: "same-origin",
      body: JSON.stringify(body),
    });
  } catch {
    throw new ApiError(0, "network", "Lake could not be reached. Check your connection and retry.");
  }
  if (response.status === 204) return undefined as T;
  const json = await response.json().catch(() => null);
  if (!response.ok || json?.ok === false) {
    const error = json?.error ?? {};
    throw new ApiError(response.status, error.code ?? "error", error.message ?? response.statusText, error.path ?? "");
  }
  refreshAfterWrite();
  return (json && "value" in json ? json.value : json) as T;
}

export const queryClient = new QueryClient({
  defaultOptions: {
    queries: {
      staleTime: 15_000,
      // A 4xx is the answer (not found, not allowed, a bad id): show it at once. Only a network
      // failure or a 5xx is worth retrying.
      retry: (count, error) => !(error instanceof ApiError && error.status >= 400 && error.status < 500) && count < 2,
      refetchOnWindowFocus: false,
    },
  },
});

/** A read capability as a query. `name` null disables it (a dependent read). */
export function useRead<T = Row>(name: string | null, input?: Params, options?: Partial<UseQueryOptions<T>> & CallOptions) {
  const { workspace, ...queryOptions } = options ?? {};
  const scope = workspace !== undefined ? workspace ?? "" : workspacePrefix;
  return useQuery<T>({
    queryKey: [name, scope, input ?? {}],
    queryFn: ({ signal }) => read<T>(name!, input, { workspace, signal }),
    ...queryOptions,
    // After the caller's options: a null name never reads, whatever `enabled` says.
    enabled: !!name && (queryOptions.enabled ?? true),
  });
}

/** Start a read now, under the key `useRead` will look for, so a page that is still loading its
 * code finds the answer already on its way. */
export function prefetchRead(name: string, input?: Params, options?: CallOptions) {
  const scope = options && options.workspace !== undefined ? (options.workspace ? `/w/${encodeURIComponent(options.workspace)}` : "") : workspacePrefix;
  void queryClient.prefetchQuery({
    queryKey: [name, scope, input ?? {}],
    queryFn: ({ signal }) => read(name, input, { workspace: options?.workspace, signal }),
  });
}

/** A read capability whose value holds a list, as a query of the list itself. `pick` names the
 * field that carries it (most lake reads answer `{<plural>: [...]}`); with no `pick` the first
 * array-valued field is taken. */
export function useList<T = Row>(
  name: string | null,
  input?: Params,
  options?: Partial<UseQueryOptions<T[]>> & CallOptions & { pick?: string },
) {
  const { workspace, pick, ...queryOptions } = options ?? {};
  const scope = workspace !== undefined ? workspace ?? "" : workspacePrefix;
  return useQuery<T[]>({
    queryKey: [name, scope, input ?? {}, pick ?? ""],
    queryFn: async ({ signal }) => listOf<T>(await read<unknown>(name!, input, { workspace, signal }), pick),
    ...queryOptions,
    enabled: !!name && (queryOptions.enabled ?? true),
  });
}

/** The list a read answered: the value itself, the `pick` field, or its first array field. */
export function listOf<T>(value: unknown, pick?: string): T[] {
  if (Array.isArray(value)) return value as T[];
  if (value && typeof value === "object") {
    const record = value as Row;
    if (pick) return (Array.isArray(record[pick]) ? record[pick] : []) as T[];
    for (const field of Object.values(record)) if (Array.isArray(field)) return field as T[];
  }
  return [];
}

/** A POSTed read as a query: `usePostRead("workspace_query", {capability: "list_projects"})`. */
export function usePostRead<T = Row>(name: string | null, input: Row = {}, options?: Partial<UseQueryOptions<T>> & CallOptions) {
  const { workspace, ...queryOptions } = options ?? {};
  const scope = workspace !== undefined ? workspace ?? "" : workspacePrefix;
  return useQuery<T>({
    queryKey: [name, scope, "post", input],
    queryFn: ({ signal }) => postRead<T>(name!, input, { workspace, signal }),
    ...queryOptions,
    enabled: !!name && (queryOptions.enabled ?? true),
  });
}

/** An action capability as a mutation. */
export function useAct<TIn extends Row = Row, TOut = Row>(name: string, options?: CallOptions) {
  return useMutation<TOut, ApiError, TIn>({ mutationFn: (input: TIn) => act<TOut>(name, input, options) });
}
