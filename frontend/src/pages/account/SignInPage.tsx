/** Sign in (`/sign-in`). Under Clerk, Clerk's form (ClerkSignIn). Otherwise lake's own: a password
 * (with the TOTP step when the account has MFA, keeping the email and password in memory) and an
 * OAuth provider. Both call `/v1` directly; the password call sets `lake_session` itself. */
import { useState } from "react";
import { Link, useLocation } from "react-router-dom";
import { act, ApiError } from "@/lib/api";
import { Button, Field, fieldError, Form, Notice, Page } from "@/lib/ui";
import { useSession } from "@/lib/session";
import { AuthFrame, useSignedInRedirect } from "./AuthFrame";
import { ClerkSignIn } from "./ClerkSignIn";
import { leaveFor, messageOf, NO_WORKSPACE, returnQuery, returnTarget } from "./signIn";

export default function SignInPage() {
  const { viewer } = useSession();
  const signedIn = useSignedInRedirect((administrator, clerk) => (administrator || clerk ? "/repositories" : "/sessions"));
  if (signedIn) return null;
  if (viewer.clerk.enabled) return <ClerkSignIn mode="sign-in" />;
  return <NativeSignIn />;
}

function NativeSignIn() {
  const location = useLocation();
  const kept = returnQuery(location.search);
  return (
    <AuthFrame title="sign in">
      <Page eyebrow="identity" title="Sign In" sub="Use a password, passkey, or configured social provider.">
        <div className="grid">
          <PasswordPanel />
          <OAuthPanel />
        </div>
        <p className="muted">
          <Link to={`/sign-up${kept}`}>Create an account</Link> · <Link to="/login">Operator sign-in</Link>
        </p>
      </Page>
    </AuthFrame>
  );
}

function PasswordPanel() {
  const location = useLocation();
  const kept = returnQuery(location.search);
  const [email, setEmail] = useState("");
  const [password, setPassword] = useState("");
  const [deviceName, setDeviceName] = useState("browser");
  const [code, setCode] = useState("");
  // The account has MFA: the second step, with the email and password kept from the first.
  const [mfa, setMfa] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<unknown>(null);

  const signIn = async () => {
    setBusy(true);
    setError(null);
    try {
      await act("sign_in_password", { email, password, device_name: deviceName }, NO_WORKSPACE);
      leaveFor(returnTarget(location.search, "/sessions"));
    } catch (e) {
      if (e instanceof ApiError && e.code === "mfa_required") setMfa(true);
      else setError(e);
      setBusy(false);
    }
  };
  const verify = async () => {
    setBusy(true);
    setError(null);
    try {
      await act("verify_mfa", { email, password, code, device_name: deviceName }, NO_WORKSPACE);
      leaveFor(returnTarget(location.search, "/sessions"));
    } catch (e) {
      setError(e);
      setBusy(false);
    }
  };

  if (mfa) {
    return (
      <div className="panel">
        <h2>Password</h2>
        <p>Verify your password with a current TOTP or unused recovery code.</p>
        {error ? <Notice tone="danger">{messageOf(error)}</Notice> : null}
        <Form onSubmit={verify}>
          <Field label="TOTP or recovery code" error={fieldError(error, "code")} required>
            <input type="text" name="code" autoComplete="one-time-code" autoFocus value={code} onChange={(e) => setCode(e.target.value)} />
          </Field>
          <div className="actions">
            <Button type="submit" variant="primary" loading={busy}>
              Verify and sign in
            </Button>
            <Button
              onClick={() => {
                setMfa(false);
                setCode("");
                setError(null);
              }}
            >
              Back
            </Button>
          </div>
        </Form>
      </div>
    );
  }

  return (
    <div className="panel">
      <h2>Password</h2>
      {error ? <Notice tone="danger">{messageOf(error)}</Notice> : null}
      <Form onSubmit={signIn}>
        <Field label="Email" error={fieldError(error, "email")} required>
          <input type="email" name="email" autoComplete="username" value={email} onChange={(e) => setEmail(e.target.value)} />
        </Field>
        <Field label="Password" error={fieldError(error, "password")} required>
          <input type="password" name="password" autoComplete="current-password" value={password} onChange={(e) => setPassword(e.target.value)} />
        </Field>
        <Field label="Device name" error={fieldError(error, "device_name")} required>
          <input type="text" name="device_name" value={deviceName} onChange={(e) => setDeviceName(e.target.value)} />
        </Field>
        <div>
          <Button type="submit" variant="primary" loading={busy}>
            Sign in
          </Button>
        </div>
      </Form>
      <p>
        <Link to={`/mfa${kept}`}>Verify TOTP</Link> · <Link to={`/passkey-sign-in${kept}`}>Use a passkey</Link>
      </p>
    </div>
  );
}

function OAuthPanel() {
  const [provider, setProvider] = useState("");
  const [deviceName, setDeviceName] = useState("browser");
  const [identityDocument, setIdentityDocument] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<unknown>(null);
  const start = async () => {
    setBusy(true);
    setError(null);
    try {
      const value = await act<{ authorization_url?: string }>(
        "start_oauth",
        { provider: provider.trim(), device_name: deviceName, ...(identityDocument.trim() ? { identity_document: identityDocument.trim() } : {}) },
        NO_WORKSPACE,
      );
      // The provider sends the person back to lake's callback, which sets the cookie.
      leaveFor(value.authorization_url || "/sign-in");
    } catch (e) {
      setError(e);
      setBusy(false);
    }
  };
  return (
    <div className="panel">
      <h2>OAuth</h2>
      {error ? <Notice tone="danger">{messageOf(error)}</Notice> : null}
      <Form onSubmit={start}>
        <Field label="Provider" error={fieldError(error, "provider")} required>
          <input type="text" name="provider" placeholder="github or google" value={provider} onChange={(e) => setProvider(e.target.value)} />
        </Field>
        <Field label="Device name" error={fieldError(error, "device_name")} required>
          <input type="text" name="device_name" value={deviceName} onChange={(e) => setDeviceName(e.target.value)} />
        </Field>
        <Field label="TAAP/1 identity document for first sign-in" error={fieldError(error, "identity_document")}>
          <textarea name="identity_document" spellCheck={false} value={identityDocument} onChange={(e) => setIdentityDocument(e.target.value)} />
        </Field>
        <div>
          <Button type="submit" variant="primary" loading={busy}>
            Continue to provider
          </Button>
        </div>
      </Form>
    </div>
  );
}
