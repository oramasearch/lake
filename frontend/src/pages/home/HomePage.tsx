/** Home (`/`): Quickstart for a signed-in person (lake#1128; Ask Orama, the home from lake#600, is at
 * `/ask`); for a visitor and the operator, what lake is for and their next step. An old Command
 * Center link (`/?ws=`) opens the Dashboard, and an old Ask Orama home link (`/?conversation=`) opens
 * `/ask`. The landing
 * copy is pinned by docs/ui/front-door.md (`WebApp.FrontDoorDocQuotesTheHomePage`); keep its bold
 * quotes word for word. */
import { Link, Navigate, useLocation } from "react-router-dom";
import { useSession } from "@/lib/session";
import { useTrail } from "@/lib/trail";
import { Actions, Button, Page } from "@/lib/ui";
import { INVITATION_NOTE, signUpByInvitation } from "@/lib/clerkSignIn";
import { useClerkSignUpMode } from "@/pages/account/ClerkSignIn";
import { homeView } from "@/pages/ask/home";
import QuickstartPage from "@/pages/quickstart/QuickstartPage";

export default function HomePage() {
  const { viewer } = useSession();
  const { search } = useLocation();
  const home = homeView(viewer, search);
  if (home.view === "redirect") return <Navigate to={home.to} replace />;
  if (home.view === "quickstart") return <QuickstartPage />;
  return <Landing />;
}

function Landing() {
  useTrail([{ label: "Home", to: "/" }, { label: "Welcome to Lake" }], "design, train, and ship AI");
  const { viewer } = useSession();
  const signedIn = viewer.signed_in;
  const clerkVisitor = !signedIn && viewer.clerk.enabled;
  const signUpMode = useClerkSignUpMode(clerkVisitor ? viewer.clerk.publishable_key : undefined, clerkVisitor ? viewer.clerk.frontend_api : undefined);
  return (
    <Page
      eyebrow="Lake by Orama"
      title="Design, train, and ship AI on your own data."
      sub="Load your data into your own workspace, train models on it, and call them from your code, in Orama's cloud or your own."
    >
      {signedIn ? (
        <Actions>
          <Button to="/docs" variant="primary">
            Getting started
          </Button>
          <Button to="/train/data">Load your data</Button>
        </Actions>
      ) : viewer.clerk.enabled ? (
        signUpByInvitation(signUpMode) ? (
          <>
            <Actions>
              <Button to="/sign-in" variant="primary">
                Sign in
              </Button>
              <Button to="/docs">Read the docs</Button>
            </Actions>
            <p className="muted">{INVITATION_NOTE}</p>
          </>
        ) : signUpMode === "public" ? (
          <Actions>
            <Button to="/sign-up" variant="primary">
              Create an account
            </Button>
            <Button to="/sign-in">Sign in</Button>
            <Button to="/docs">Read the docs</Button>
          </Actions>
        ) : (
          <Actions>
            <Button to="/sign-in" variant="primary">
              Sign in
            </Button>
            <Button to="/docs">Read the docs</Button>
          </Actions>
        )
      ) : viewer.password_sign_up ? (
        <Actions>
          <Button to="/sign-up" variant="primary">
            Create account
          </Button>
          <Button to="/sign-in">Sign in</Button>
          <Button to="/docs">Read the docs</Button>
        </Actions>
      ) : (
        <Actions>
          <Button to="/sign-in" variant="primary">
            Sign in
          </Button>
          <Button to="/docs">Read the docs</Button>
        </Actions>
      )}
      <div className="grid">
        <section className="card">
          <h3>Design</h3>
          <p>Start from the business question and the decision it should improve. A project turns it into a pipeline you can read, change and approve.</p>
          {signedIn && (
            <p>
              <Link to="/design/projects">Projects</Link>
            </p>
          )}
        </section>
        <section className="card">
          <h3>Train</h3>
          <p>
            Your data lives in your workspace: upload a file, connect a database or generate a dataset, then train on it and read what each run found.
          </p>
          {signedIn && (
            <p>
              <Link to="/train/data">Data</Link> · <Link to="/train/runs">Runs</Link>
            </p>
          )}
        </section>
        <section className="card">
          <h3>Ship</h3>
          <p>Publish a model and call it from your code, publish a search index, or run databases for your team. Files and code live in repositories.</p>
          {signedIn && (
            <p>
              <Link to="/ship/models">Models</Link> · <Link to="/repositories">Repositories</Link>
            </p>
          )}
          {signedIn && !viewer.administrator && (
            <p>
              <Link to="/tokens">Create a token</Link>
            </p>
          )}
        </section>
      </div>
    </Page>
  );
}
