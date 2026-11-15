// SPDX-License-Identifier: MIT

/** The router: every registered screen, inside the shell unless it is bare (sign-in, public share
 * pages). Workspace pages answer at `/w/:workspaceId<path>`; their unprefixed address opens the
 * remembered or only workspace, or asks for one. */
import { Suspense, useEffect, type ComponentType } from "react";
import { Navigate, Outlet, Route, Routes, useLocation, useParams } from "react-router-dom";
import { SessionProvider, useSession } from "@/lib/session";
import { TrailProvider } from "@/lib/trail";
import { Page, Skeleton, Empty, Button } from "@/lib/ui";
import { audienceOf, SCREENS, type Screen } from "@/routes";
import { billingVisible } from "@/routes/nav";
import { Shell } from "@/components/Shell";
import { SessionEndedNotice } from "@/components/SessionEnded";
import { ScreenBoundary } from "@/components/ScreenBoundary";
import { preloadPages } from "@/lib/lazyPage";

function Loading() {
  return (
    <div className="page-container">
      <Skeleton />
    </div>
  );
}

/** Who may open a screen: a visitor opening an account page is sent to sign in, and back. */
function Guard({ screen, children }: { screen: Screen; children: React.ReactNode }) {
  const session = useSession();
  const location = useLocation();
  const audience = screen.audience ?? ["account", "operator"];
  const who = audienceOf(session);
  if (who !== "visitor" && !billingVisible(screen, session.viewer)) {
    return (
      <Page title="Not Available">
        <Empty action={<Button to="/usage">Usage</Button>}>This Lake does not bill. Usage is tracked as usual.</Empty>
      </Page>
    );
  }
  if (!audience.includes(who)) {
    if (who === "visitor") {
      return <Navigate to={`/sign-in?next=${encodeURIComponent(location.pathname + location.search)}`} replace />;
    }
    return (
      <Page title="Not Available">
        <Empty action={<Button to="/">Home</Button>}>
          {who === "operator" ? "This page belongs to an account; the operator credential has no account." : "This page is for the Lake operator."}
        </Empty>
      </Page>
    );
  }
  return <>{children}</>;
}

/** An unprefixed workspace page: open it in the remembered or only workspace, else ask. */
function WorkspaceRedirect({ screen }: { screen: Screen }) {
  const { workspaceId, workspaces, viewer } = useSession();
  const location = useLocation();
  if (!viewer.signed_in) return <Navigate to={`/sign-in?next=${encodeURIComponent(location.pathname + location.search)}`} replace />;
  const id = workspaceId ?? (workspaces.length === 1 ? workspaces[0].id : null);
  if (id) return <Navigate to={`/w/${encodeURIComponent(id)}${screen.path}${location.search}`} replace />;
  return (
    <Page title="Choose a Workspace" sub="Select a workspace above or create a new one.">
      <Button to="/workspaces" variant="primary">
        Workspaces
      </Button>
    </Page>
  );
}

/** `/w/<id>` and `/w/<id>/`: the workspace's home. */
function WorkspaceHome() {
  const { workspaceId } = useParams();
  const location = useLocation();
  return <Navigate to={`/w/${workspaceId}/design/projects${location.search}`} replace />;
}

function page(screen: Screen) {
  const Component = screen.component as ComponentType<object>;
  return (
    <Guard screen={screen}>
      <Component />
    </Guard>
  );
}

/** Bare pages (sign-in, public share links) render without the shell. */
function bare(screen: Screen) {
  return (
    <ScreenBoundary>
      <Suspense fallback={<Loading />}>{page(screen)}</Suspense>
    </ScreenBoundary>
  );
}

/** One shell for every console page: it stays mounted across navigation, so the rail, the top bar
 * and their state survive, and only the page area waits for a page's code. */
function ShellLayout() {
  return (
    <Shell>
      <Outlet />
    </Shell>
  );
}

export default function App() {
  // Once the first page is up, fetch the other pages' code while the browser is idle.
  useEffect(() => {
    const timer = setTimeout(preloadPages, 1500);
    return () => clearTimeout(timer);
  }, []);
  return (
    <TrailProvider>
      <SessionProvider fallback={<Loading />}>
        <Routes>
          <Route path="/w/:workspaceId" element={<WorkspaceHome />} />
          <Route path="/w/:workspaceId/" element={<WorkspaceHome />} />
          {SCREENS.filter((screen) => screen.bare).map((screen) => (
            <Route key={screen.path} path={screen.path} element={bare(screen)} />
          ))}
          <Route element={<ShellLayout />}>
            {SCREENS.filter((screen) => !screen.bare).flatMap((screen) =>
              screen.workspace
                ? [
                    <Route key={`w${screen.path}`} path={`/w/:workspaceId${screen.path}`} element={page(screen)} />,
                    <Route key={screen.path} path={screen.path} element={<WorkspaceRedirect screen={screen} />} />,
                  ]
                : [<Route key={screen.path} path={screen.path} element={page(screen)} />],
            )}
          </Route>
        </Routes>
        <SessionEndedNotice />
      </SessionProvider>
    </TrailProvider>
  );
}
