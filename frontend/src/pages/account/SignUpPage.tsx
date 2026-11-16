// SPDX-License-Identifier: MIT

/** Create an account (`/sign-up`). Under Clerk, Clerk's sign-up with the invitation sentence;
 * otherwise lake's own password sign-up, which gives the account a managed key, provisions the
 * person's own space and sets `lake_session` (`sign_up_password`). No identity document is asked
 * for: the person may use their own identity later, in Account → Identity (lake#762). */
import { useState } from "react";
import { Link, useLocation } from "react-router-dom";
import { act } from "@/lib/api";
import { Button, Field, fieldError, Form, Notice, Page } from "@/lib/ui";
import { useSession } from "@/lib/session";
import { AuthFrame, useSignedInRedirect } from "./AuthFrame";
import { ClerkSignIn } from "./ClerkSignIn";
import { LegalNotice } from "./LegalNotice";
import { SsoSignIn } from "./SsoSignIn";
import { AccountRegionNote } from "@/components/Region";
import { landingAfterSignIn, leaveFor, messageOf, NO_WORKSPACE, returnQuery, returnTarget } from "./signIn";

export default function SignUpPage() {
  const { viewer } = useSession();
  const signedIn = useSignedInRedirect((administrator) => landingAfterSignIn(administrator));
  if (signedIn) return null;
  if (viewer.clerk.enabled) return <ClerkSignIn mode="sign-up" />;
  if (viewer.sign_in_sso) return <SsoSignIn provider={viewer.sign_in_sso} mode="sign-up" />;
  return <NativeSignUp />;
}

function NativeSignUp() {
  const location = useLocation();
  const [email, setEmail] = useState("");
  const [displayName, setDisplayName] = useState("");
  const [password, setPassword] = useState("");
  const [deviceName, setDeviceName] = useState("browser");
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
        <Notice tone="info">
          Lake gives your account a managed key, so you need nothing else to start. To sign with your own liquid identity instead, open Account → Identity after you sign in.
        </Notice>
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
          <div>
            <Button type="submit" variant="primary" loading={busy}>
              Create account
            </Button>
          </div>
        </Form>
        <LegalNotice mode="sign-up" />
        <p className="muted">
          Already registered? <Link to={`/sign-in${returnQuery(location.search)}`}>Sign in</Link>.
        </p>
      </Page>
    </AuthFrame>
  );
}
