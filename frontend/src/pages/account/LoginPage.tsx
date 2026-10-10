/** Operator sign-in (`/login`). A real form POST to `/session`: the server compares the credential,
 * sets the HttpOnly operator cookie and redirects. No capability issues that cookie, and the
 * credential never passes through the page's script. */
import { Link, useLocation } from "react-router-dom";
import { Notice, Page } from "@/lib/ui";
import { AuthFrame, useSignedInRedirect } from "./AuthFrame";

export default function LoginPage() {
  const location = useLocation();
  const signedIn = useSignedInRedirect(() => "/repositories");
  const error = new URLSearchParams(location.search).get("error");
  if (signedIn) return null;
  return (
    <AuthFrame title="operator sign in">
      <Page
        eyebrow="Operator access"
        title="Operator Sign In"
        sub={
          <>
            Use your operator credential to manage this Lake. For your own repositories, <Link to="/sign-in">sign in to your account</Link>.
          </>
        }
      >
        {error && <Notice tone="danger">{error}</Notice>}
        <form className="login" method="post" action="/session">
          <label>
            Operator credential
            <input type="password" name="token" placeholder="Operator credential" autoComplete="current-password" required />
          </label>
          <button type="submit">Sign in</button>
        </form>
      </Page>
    </AuthFrame>
  );
}
