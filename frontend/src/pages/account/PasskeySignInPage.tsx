// SPDX-License-Identifier: MIT

/** Sign in with a passkey (`/passkey-sign-in`): the WebAuthn assertion, then the pending return or
 * Sessions, as the server page did. */
import { useState } from "react";
import { useLocation } from "react-router-dom";
import { Page } from "@/lib/ui";
import { AuthFrame, useSignedInRedirect } from "./AuthFrame";
import { landingAfterSignIn, leaveFor, messageOf, returnTarget } from "./signIn";
import { signInWithPasskey } from "./webauthn";

export default function PasskeySignInPage() {
  const location = useLocation();
  const signedIn = useSignedInRedirect((administrator) => landingAfterSignIn(administrator));
  const [deviceName, setDeviceName] = useState("browser");
  const [status, setStatus] = useState("");
  const [busy, setBusy] = useState(false);
  if (signedIn) return null;
  const use = async () => {
    setBusy(true);
    setStatus("");
    try {
      await signInWithPasskey(deviceName);
      leaveFor(returnTarget(location.search, landingAfterSignIn(false)));
    } catch (e) {
      setStatus(messageOf(e));
      setBusy(false);
    }
  };
  return (
    <AuthFrame title="passkey sign in">
      <Page eyebrow="identity" title="Sign In with a Passkey">
        <div className="panel">
          <label>
            Device name <input id="device-name" type="text" value={deviceName} onChange={(e) => setDeviceName(e.target.value)} />
          </label>
          <div className="actions">
            <button className="primary" id="use-passkey" type="button" onClick={use} disabled={busy}>
              Use passkey
            </button>
            <span id="passkey-status" className="muted" role="status" aria-live="polite">
              {status}
            </span>
          </div>
        </div>
      </Page>
    </AuthFrame>
  );
}
