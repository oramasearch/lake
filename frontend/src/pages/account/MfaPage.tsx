/** Multi-factor authentication (`/mfa`). Signed in: enroll or replace a TOTP authenticator (the URI
 * and recovery codes are shown once, held only in this page's memory) and disable it. A visitor:
 * the password-and-code challenge, which signs in. */
import { useState } from "react";
import { Navigate, useLocation } from "react-router-dom";
import { act } from "@/lib/api";
import { useSession } from "@/lib/session";
import { useToast } from "@/lib/toast";
import { useTrail } from "@/lib/trail";
import { Button, Field, fieldError, Form, Notice, Page } from "@/lib/ui";
import { leaveFor, messageOf, NO_WORKSPACE, returnTarget } from "./signIn";
import "@/styles/account.css";

export default function MfaPage() {
  const { viewer } = useSession();
  const operator = viewer.signed_in && viewer.administrator;
  // The operator credential has no account and no MFA.
  if (operator) return <Navigate to="/repositories" replace />;
  return viewer.signed_in ? <Manage /> : <Challenge />;
}

type Enrollment = { otpauth_uri: string; recovery_codes: string[] };

function Manage() {
  useTrail([{ label: "Account", to: "/settings" }, { label: "MFA" }]);
  const toast = useToast();
  const [enrollment, setEnrollment] = useState<Enrollment | null>(null);
  const [enrolling, setEnrolling] = useState(false);
  const [code, setCode] = useState("");
  const [disabling, setDisabling] = useState(false);
  const [error, setError] = useState<unknown>(null);

  const enroll = async () => {
    setEnrolling(true);
    setError(null);
    try {
      setEnrollment(await act<Enrollment>("enroll_mfa", {}, NO_WORKSPACE));
    } catch (e) {
      setError(e);
    } finally {
      setEnrolling(false);
    }
  };
  const disable = async () => {
    setDisabling(true);
    setError(null);
    try {
      await act("disable_mfa", { code }, NO_WORKSPACE);
      setEnrollment(null);
      setCode("");
      toast("MFA disabled");
    } catch (e) {
      setError(e);
    } finally {
      setDisabling(false);
    }
  };

  return (
    <Page
      eyebrow="identity"
      title="Multi-Factor Authentication"
      sub="Enroll or replace a TOTP authenticator for password sign-in. A new enrollment replaces existing recovery codes."
    >
      {error ? <Notice tone="danger">{messageOf(error)}</Notice> : null}
      {enrollment && (
        <>
          <p className="notice">
            Save this authenticator URI now: <code>{enrollment.otpauth_uri}</code>
          </p>
          <h2>Recovery Codes</h2>
          <p>Save these now. Each code works once.</p>
          <ul className="recovery-codes">
            {enrollment.recovery_codes.map((recovery) => (
              <li key={recovery}>
                <code>{recovery}</code>
              </li>
            ))}
          </ul>
        </>
      )}
      <form
        className="stack"
        onSubmit={(e) => {
          e.preventDefault();
          void enroll();
        }}
      >
        <div>
          <Button type="submit" variant="primary" loading={enrolling}>
            Enroll or replace MFA
          </Button>
        </div>
      </form>
      <h2>Disable MFA</h2>
      <Form onSubmit={disable}>
        <Field label="Current TOTP or recovery code" error={fieldError(error, "code")} required>
          <input type="text" name="code" autoComplete="one-time-code" value={code} onChange={(e) => setCode(e.target.value)} />
        </Field>
        <div>
          <Button type="submit" loading={disabling}>
            Disable MFA
          </Button>
        </div>
      </Form>
    </Page>
  );
}

function Challenge() {
  useTrail([]);
  const location = useLocation();
  const [email, setEmail] = useState("");
  const [password, setPassword] = useState("");
  const [code, setCode] = useState("");
  const [deviceName, setDeviceName] = useState("browser");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<unknown>(null);
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
  return (
    <Page eyebrow="identity" title="Multi-Factor Authentication" sub="Verify your password with a current TOTP or unused recovery code.">
      {error ? <Notice tone="danger">{messageOf(error)}</Notice> : null}
      <Form onSubmit={verify}>
        <Field label="Email" error={fieldError(error, "email")} required>
          <input type="email" name="email" autoComplete="username" value={email} onChange={(e) => setEmail(e.target.value)} />
        </Field>
        <Field label="Password" error={fieldError(error, "password")} required>
          <input type="password" name="password" autoComplete="current-password" value={password} onChange={(e) => setPassword(e.target.value)} />
        </Field>
        <Field label="TOTP or recovery code" error={fieldError(error, "code")} required>
          <input type="text" name="code" autoComplete="one-time-code" value={code} onChange={(e) => setCode(e.target.value)} />
        </Field>
        <Field label="Device name" error={fieldError(error, "device_name")} required>
          <input type="text" name="device_name" value={deviceName} onChange={(e) => setDeviceName(e.target.value)} />
        </Field>
        <div>
          <Button type="submit" variant="primary" loading={busy}>
            Verify and sign in
          </Button>
        </div>
      </Form>
    </Page>
  );
}
