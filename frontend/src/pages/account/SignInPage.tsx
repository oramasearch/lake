/** Sign in (`/sign-in`). Under Clerk, Clerk's form (ClerkSignIn). Otherwise lake's own: a password
 * (with the TOTP step when the account has MFA, keeping the email and password in memory) and an
 * OAuth provider. Both call `/v1` directly; the password call sets `lake_session` itself. An OAuth
 * first sign-in with an identity document proves its key first (lake#931). */
import { useState } from "react";
import { Link, useLocation } from "react-router-dom";
import { act, ApiError } from "@/lib/api";
import { Button, CopyBlock, Field, fieldError, Form, Notice, Page } from "@/lib/ui";
import { type IdentityProofChallenge, looksSigned, signCommand, signedOutput } from "@/lib/identity";
import { useSession } from "@/lib/session";
import { AuthFrame, useSignedInRedirect } from "./AuthFrame";
import { ClerkSignIn } from "./ClerkSignIn";
import { LegalNotice } from "./LegalNotice";
import { AccountRegionNote } from "@/components/Region";
import { landingAfterSignIn, leaveFor, messageOf, NO_WORKSPACE, returnQuery, returnTarget } from "./signIn";

export default function SignInPage() {
  const { viewer } = useSession();
  const signedIn = useSignedInRedirect((administrator) => landingAfterSignIn(administrator));
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
        <AccountRegionNote mode="sign-in" />
        <div className="grid">
          <PasswordPanel />
          <OAuthPanel />
        </div>
        <LegalNotice mode="sign-in" />
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
      leaveFor(returnTarget(location.search, landingAfterSignIn(false)));
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
      leaveFor(returnTarget(location.search, landingAfterSignIn(false)));
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

/** A first sign-in that brings an identity document proves the person holds its key (lake#931): Lake
 * issues a challenge for the document (`begin_identity_proof`), the person signs it with `liquid sign
 * artifact`, and `start_oauth` carries what that printed as `identity_proof`. */
function OAuthPanel() {
  const [provider, setProvider] = useState("");
  const [deviceName, setDeviceName] = useState("browser");
  const [identityDocument, setIdentityDocument] = useState("");
  const [proof, setProof] = useState<IdentityProofChallenge | null>(null);
  const [signature, setSignature] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<unknown>(null);
  const document = identityDocument.trim();
  const start = async () => {
    setBusy(true);
    setError(null);
    try {
      if (document && !proof) {
        setProof(await act<IdentityProofChallenge>("begin_identity_proof", { identity_document: document }, NO_WORKSPACE));
        setBusy(false);
        return;
      }
      const value = await act<{ authorization_url?: string }>(
        "start_oauth",
        {
          provider: provider.trim(),
          device_name: deviceName,
          ...(document && proof
            ? { identity_document: document, identity_proof: { nonce: proof.nonce, expires_at: proof.expires_at, signature: signedOutput(signature) } }
            : {}),
        },
        NO_WORKSPACE,
      );
      // The provider sends the person back to lake's callback, which sets the cookie.
      leaveFor(value.authorization_url || "/sign-in");
    } catch (e) {
      setError(e);
      // A refused proof or an expired challenge needs a new challenge.
      if (proof) {
        setProof(null);
        setSignature("");
      }
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
          <textarea
            name="identity_document"
            spellCheck={false}
            value={identityDocument}
            readOnly={proof !== null}
            onChange={(e) => setIdentityDocument(e.target.value)}
          />
        </Field>
        {proof ? (
          <>
            <p className="m-0 text-[13px]">Prove you hold this identity's key: sign this challenge before it expires, and paste everything the command prints.</p>
            <div className="min-w-0 [&_pre]:break-all [&_pre]:whitespace-pre-wrap">
              <CopyBlock label="Run" value={signCommand(proof.challenge)} multiline />
            </div>
            <Field label="What liquid sign printed" error={fieldError(error, "identity_proof")} required>
              <textarea name="identity_signature" spellCheck={false} value={signature} onChange={(e) => setSignature(e.target.value)} />
            </Field>
          </>
        ) : null}
        <div className="flex gap-2">
          <Button type="submit" variant="primary" loading={busy} disabled={proof !== null && !looksSigned(signature)}>
            {document && !proof ? "Continue" : "Continue to provider"}
          </Button>
          {proof ? <Button onClick={() => { setProof(null); setSignature(""); }}>Start again</Button> : null}
        </div>
      </Form>
    </div>
  );
}
