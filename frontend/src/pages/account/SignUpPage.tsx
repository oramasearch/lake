/** Create an account (`/sign-up`). Under Clerk, Clerk's sign-up with the invitation sentence;
 * otherwise lake's own password sign-up with a TAAP/1 identity document, which provisions the
 * person's own space and sets `lake_session` (`sign_up_password`). */
import { useState } from "react";
import { Link, useLocation } from "react-router-dom";
import { act } from "@/lib/api";
import { Button, Field, fieldError, Form, Notice, Page } from "@/lib/ui";
import { useSession } from "@/lib/session";
import { AuthFrame, useSignedInRedirect } from "./AuthFrame";
import { ClerkSignIn } from "./ClerkSignIn";
import { AccountRegionNote } from "@/components/Region";
import { landingAfterSignIn, leaveFor, messageOf, NO_WORKSPACE, returnQuery, returnTarget } from "./signIn";

const INSTALL = `git clone --recurse-submodules git@github.com:oramasearch/liquid.git
cd liquid
cmake -S . -B build -G Ninja -DLIQUID_BUILD_TESTS=OFF
cmake --build build -j 6
cmake --install build --prefix "$HOME/.local"
export PATH="$HOME/.local/bin:$PATH"`;

export default function SignUpPage() {
  const { viewer } = useSession();
  const signedIn = useSignedInRedirect((administrator) => landingAfterSignIn(administrator));
  if (signedIn) return null;
  if (viewer.clerk.enabled) return <ClerkSignIn mode="sign-up" />;
  return <NativeSignUp />;
}

function NativeSignUp() {
  const location = useLocation();
  const [email, setEmail] = useState("");
  const [displayName, setDisplayName] = useState("");
  const [password, setPassword] = useState("");
  const [deviceName, setDeviceName] = useState("browser");
  const [identityDocument, setIdentityDocument] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<unknown>(null);

  const submit = async () => {
    setBusy(true);
    setError(null);
    try {
      await act(
        "sign_up_password",
        {
          email,
          password,
          device_name: deviceName,
          identity_document: identityDocument.trim(),
          ...(displayName.trim() ? { display_name: displayName.trim() } : {}),
        },
        NO_WORKSPACE,
      );
      leaveFor(returnTarget(location.search, "/repositories"));
    } catch (e) {
      setError(e);
      setBusy(false);
    }
  };

  return (
    <AuthFrame title="sign up">
      <Page eyebrow="identity" title="Create an Account" sub="Get a personal customer you own, then create your first repository.">
        <AccountRegionNote mode="sign-up" />
        {error ? <Notice tone="danger">{messageOf(error)}</Notice> : null}
        <section className="panel">
          <h2>Get Your Public Identity Document</h2>
          <ol>
            <li>
              <a href="https://github.com/oramasearch/liquid/blob/main/docs/build/local.md">Install liquid</a> using the supported build instructions. You need access to
              the liquid repository.
              <details>
                <summary>Installation commands</summary>
                <pre>
                  <code>{INSTALL}</code>
                </pre>
              </details>
            </li>
            <li>
              Run <code>liquid identity show --document</code>. Liquid creates your identity on first use, or exports your existing identity's public document.{" "}
              <a href="https://github.com/oramasearch/liquid/blob/main/docs/reference/cli/identity.md">Identity command reference</a>.
            </li>
            <li>Copy the complete output into the identity field below, without quotes. It is one base64url string containing the public TAAP/1 document, not JSON.</li>
          </ol>
          <p>Your private signing key stays on your machine. Keep it safe; never paste it here.</p>
        </section>
        <Form onSubmit={submit}>
          <Field label="Email" error={fieldError(error, "email")} required>
            <input type="email" name="email" autoComplete="email" value={email} onChange={(e) => setEmail(e.target.value)} />
          </Field>
          <Field label="Display name" error={fieldError(error, "display_name")}>
            <input type="text" name="display_name" autoComplete="name" value={displayName} onChange={(e) => setDisplayName(e.target.value)} />
          </Field>
          <Field label="Password" error={fieldError(error, "password")} required>
            <input type="password" name="password" autoComplete="new-password" minLength={12} value={password} onChange={(e) => setPassword(e.target.value)} />
          </Field>
          <Field label="Device name" error={fieldError(error, "device_name")} required>
            <input type="text" name="device_name" value={deviceName} onChange={(e) => setDeviceName(e.target.value)} />
          </Field>
          <Field label="TAAP/1 identity document" error={fieldError(error, "identity_document")} required>
            <textarea name="identity_document" spellCheck={false} value={identityDocument} onChange={(e) => setIdentityDocument(e.target.value)} />
          </Field>
          <div>
            <Button type="submit" variant="primary" loading={busy}>
              Create account
            </Button>
          </div>
        </Form>
        <p className="muted">
          Already registered? <Link to={`/sign-in${returnQuery(location.search)}`}>Sign in</Link>.
        </p>
      </Page>
    </AuthFrame>
  );
}
