/** Who is viewing, and in which space and workspace. One read, `viewer`, answers it: the same
 * resolved viewer the server used to render navigation (signed in, operator, the spaces and
 * workspaces the person can choose, whether sign-in is Clerk). The workspace in the URL
 * (`/w/<id>/…`) is the context for Design, Train and Ship; the server enforces scope regardless. */
import { createContext, type ReactNode, useContext, useEffect, useMemo } from "react";
import { useLocation } from "react-router-dom";
import { setSignedIn, setWorkspace, useRead } from "./api";

/** A workspace the viewer can choose. */
export type WorkspaceChoice = { id: string; name: string; customer_id: string; status?: string };
/** A space (customer) the viewer can act in, with its live workspaces. "Personal space" is the
 * viewer's own individual customer. */
export type SpaceChoice = { id: string; name: string; label: string; personal: boolean; kind?: string; workspaces: WorkspaceChoice[] };

export type Viewer = {
  signed_in: boolean;
  administrator: boolean;
  user_id?: string;
  handle?: string;
  display_name?: string;
  email?: string;
  taap_id?: string;
  session_id?: string;
  credential_kind?: string;
  mfa_enrolled?: boolean;
  /** The current space: the workspace's, else the only or first one. */
  customer_id?: string;
  /** The workspace asked for (validated), else the only workspace of the space. */
  workspace_id?: string;
  spaces: SpaceChoice[];
  clerk: { enabled: boolean; publishable_key?: string; frontend_api?: string };
  password_sign_up: boolean;
  /** The origin repositories are cloned from and packages installed from. */
  external_origin?: string;
  public_host?: string;
  /** Lake configuration the forms need (the agent models offered by "Create an Agent"). */
  config?: { base_agent_model?: string; frontier_model?: string };
  version?: string;
  revision?: string;
};

export type Session = {
  viewer: Viewer;
  /** The workspace the URL names, else the viewer's current one. */
  workspaceId: string | null;
  workspace: WorkspaceChoice | null;
  /** Every workspace across the viewer's spaces. */
  workspaces: WorkspaceChoice[];
  /** The space of the current workspace, else the viewer's current space. */
  space: SpaceChoice | null;
  refresh: () => void;
};

const Ctx = createContext<Session | null>(null);

const ANONYMOUS: Viewer = { signed_in: false, administrator: false, spaces: [], clerk: { enabled: false }, password_sign_up: false };

const REMEMBERED = "lake-workspace";
/** The workspace this browser last used: navigation memory only, never an API default. */
export function rememberedWorkspace(): string | null {
  try {
    return localStorage.getItem(REMEMBERED);
  } catch {
    return null;
  }
}
function remember(id: string) {
  try {
    localStorage.setItem(REMEMBERED, id);
  } catch {
    /* ignore */
  }
}

/** The workspace id in a page path: `/w/<id>/…`. */
export function workspaceFromPath(pathname: string): string | null {
  const match = /^\/w\/([^/]+)/.exec(pathname);
  return match ? decodeURIComponent(match[1]) : null;
}

export function SessionProvider({ children, fallback }: { children: ReactNode; fallback: ReactNode }) {
  const location = useLocation();
  const fromPath = workspaceFromPath(location.pathname);
  const asked = fromPath ?? rememberedWorkspace();
  const q = useRead<Viewer>("viewer", asked ? { workspace_id: asked } : undefined, { workspace: null, staleTime: 60_000 });
  const viewer = q.data ?? (q.isError ? ANONYMOUS : null);
  const workspaceId = fromPath ?? viewer?.workspace_id ?? null;
  // Every capability called from a workspace page (/w/<id>/…) is called under the same prefix.
  // Other pages call unscoped: the registry fills `workspace_id` from the prefix and refuses a
  // different one, so a remembered workspace must never scope them.
  setWorkspace(fromPath);
  useEffect(() => {
    setSignedIn(!!viewer?.signed_in);
  }, [viewer?.signed_in]);
  useEffect(() => {
    if (fromPath) remember(fromPath);
  }, [fromPath]);
  const value = useMemo<Session | null>(() => {
    if (!viewer) return null;
    const workspaces = (viewer.spaces ?? []).flatMap((s) => s.workspaces ?? []);
    const workspace = workspaces.find((w) => w.id === workspaceId) ?? null;
    const spaceId = workspace?.customer_id ?? viewer.customer_id;
    return {
      viewer,
      workspaceId,
      workspace,
      workspaces,
      space: viewer.spaces.find((s) => s.id === spaceId) ?? null,
      refresh: () => void q.refetch(),
    };
  }, [viewer, workspaceId, q]);
  if (!value) return <>{fallback}</>;
  return <Ctx.Provider value={value}>{children}</Ctx.Provider>;
}

export function useSession(): Session {
  const session = useContext(Ctx);
  if (!session) throw new Error("useSession outside SessionProvider");
  return session;
}
