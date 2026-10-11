/** The workspace gate shared by Design, Train and Ship pages (lake's web_ui_workspace and
 * workspace_gate, web_ui.cpp): why the page is unavailable (no space, no workspace control plane, a
 * failed status read), the gate while the workspace Zetta is not ready (failed, being prepared, or
 * Start when it was never asked for), otherwise the page. It reads `workspace_status` itself and
 * checks again every 15 seconds while the workspace is being prepared.
 *
 *   <Page …><WorkspaceGate>{content}</WorkspaceGate></Page>
 *   <WorkspaceGate before={notice} after={library}>{(state) => content}</WorkspaceGate>
 *
 * - `children` may be a function of the workspace's state.
 * - `before` is shown ahead of the gate or the content (the Data page's store notice).
 * - `after` is shown after either, ready or not, unless the page is unavailable (the Agents page's
 *   Agent Library, the Data page's Storage).
 * - `always` shows the content itself whether the workspace is ready or not (the Agent Library
 *   page, which is lake's, not the workspace's).
 * - `returnTo` is where Start lands; by default the current page's list page when lake allowed it
 *   (Projects, Data, Agents, Runs, Models, Search Indexes), else Projects. */
import { type ReactNode, useState } from "react";
import { useLocation, useNavigate } from "react-router-dom";
import { act, useRead, type ApiError } from "@/lib/api";
import { useSession } from "@/lib/session";
import { Boxes, Loader2, Rocket, ServerOff } from "lucide-react";
import { Banner, Button, Card, Empty, Skeleton, Status } from "@/lib/ui";
import { startReturn, START_RETURNS } from "@/pages/design/logic";

export type WorkspaceState = { requested?: boolean; phase?: string; message?: string; ready?: boolean; workspace_id?: string };

/** What lake answers when it has no workspace control plane (workspace_capabilities.cpp). */
const NOT_CONFIGURED = "workspaces are not configured on this lake";

function localPath(pathname: string): string {
  const match = /^\/w\/[^/]+(\/.*)?$/.exec(pathname);
  return match ? match[1] || "/" : pathname;
}

/** `workspace_status` for the current workspace, polled every 15 seconds while it is prepared. */
export function useWorkspaceStatus() {
  const { space, workspaceId, viewer } = useSession();
  const customerId = space?.id ?? viewer.customer_id ?? "";
  const q = useRead<WorkspaceState>(
    "workspace_status",
    { customer_id: customerId, workspace_id: workspaceId ?? "" },
    {
      enabled: !!customerId && !!workspaceId,
      refetchInterval: (query) => {
        const state = query.state.data as WorkspaceState | undefined;
        return state && state.requested && !state.ready && state.phase !== "Failed" ? 15_000 : false;
      },
    },
  );
  return { q, customerId, workspaceId };
}

export function WorkspaceGate({
  children,
  before,
  after,
  always,
  returnTo,
}: {
  children: ReactNode | ((state: WorkspaceState) => ReactNode);
  before?: ReactNode;
  after?: ReactNode;
  always?: boolean;
  returnTo?: string;
}) {
  const { q, customerId, workspaceId } = useWorkspaceStatus();
  if (!customerId) {
    return (
      <Empty icon={Boxes} title="No Workspace Yet" action={<Button to="/workspaces">Workspaces</Button>}>
        You have no workspace yet.
      </Empty>
    );
  }
  if (!workspaceId || q.isLoading) return <Skeleton />;
  if (q.isError) {
    const message = (q.error as ApiError)?.message ?? "";
    // Lake showed the reason in an empty panel, and nothing else.
    return (
      <Empty icon={ServerOff} title={message === NOT_CONFIGURED ? "Not Available on This Lake" : "Workspace Unavailable"}>
        {message === NOT_CONFIGURED ? "Design, Train and Ship are not available on this lake yet: it has no workspace control plane configured." : message}
      </Empty>
    );
  }
  const state = q.data ?? {};
  return (
    <>
      {before}
      {always || state.ready ? (typeof children === "function" ? children(state) : children) : <GatePanel state={state} customerId={customerId} returnTo={returnTo} />}
      {after}
    </>
  );
}

function GatePanel({ state, customerId, returnTo }: { state: WorkspaceState; customerId: string; returnTo?: string }) {
  const { workspaceId, refresh } = useSession();
  const location = useLocation();
  const navigate = useNavigate();
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);

  if (state.requested) {
    if (state.phase === "Failed") {
      const message = state.message ?? "";
      // The operator refuses a new workspace once the Lake holds its maximum, and says so as
      // "workspace quota reached (<n>)".
      const quota = message.startsWith("workspace quota reached");
      return (
        <div className="space-y-3">
          <Banner tone="warn" title="Your Workspace Could Not Be Prepared">
            {quota
              ? `This lake already runs as many workspaces as it allows (${message}). An operator can raise the limit; yours starts as soon as there is room, or cancel the request below.`
              : message}
          </Banner>
          <Card
            title="Cancel This Workspace"
            actions={<Button to={`/workspaces?customer_id=${encodeURIComponent(customerId)}`}>Cancel workspace</Button>}
          >
            <p className="m-0 text-[13px] text-ink-2">Withdraw the request so you can start again later.</p>
          </Card>
        </div>
      );
    }
    return (
      <div role="status">
        <Empty icon={Spinner} title="Preparing Your Workspace" action={<Status value={state.phase || "Not picked up yet"} tone="warning" />}>
          lake is preparing a workspace of your own, where your projects, data and models live apart from every other customer's.{" "}
          {state.phase ? `Status: ${state.phase}.` : "It has not been picked up yet."}
          <div className="mt-1 text-[12px] text-ink-3">This page checks again every 15 seconds.</div>
        </Empty>
      </div>
    );
  }

  const start = async () => {
    setBusy(true);
    setError(null);
    try {
      await act("start_workspace", { customer_id: customerId });
      refresh();
      // Lake landed on the section's list page when it was one of these, else on Projects.
      const destination = returnTo && START_RETURNS.includes(returnTo) ? returnTo : startReturn(localPath(location.pathname));
      const target = workspaceId ? `/w/${encodeURIComponent(workspaceId)}${destination}` : destination;
      if (target !== location.pathname) navigate(target);
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setBusy(false);
    }
  };

  return (
    <div className="space-y-3">
      {error && <Banner tone="warn">{error}</Banner>}
      <Empty
        icon={Rocket}
        title="Start Your Workspace"
        action={
          <Button variant="primary" loading={busy} onClick={() => void start()}>
            <Rocket size={14} aria-hidden /> Start your workspace
          </Button>
        }
      >
        Design, Train and Ship run in a workspace of your own, so your projects, data and models are never shared with another customer. It takes a minute or two to
        prepare.
      </Empty>
    </div>
  );
}

/** A spinning loader as an Empty state's icon. */
function Spinner({ size }: { size?: number }) {
  return <Loader2 size={size} className="animate-spin" aria-hidden />;
}
