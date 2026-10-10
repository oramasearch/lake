/** Passkeys (`/passkeys`): register an attestation-less passkey for this account. Registered
 * passkeys appear as devices on Sessions. */
import { useState } from "react";
import { Page } from "@/lib/ui";
import { useTrail } from "@/lib/trail";
import { messageOf } from "./signIn";
import { registerPasskey } from "./webauthn";

export default function PasskeysPage() {
  useTrail([{ label: "Account", to: "/settings" }, { label: "Passkeys" }]);
  const [name, setName] = useState("passkey");
  const [status, setStatus] = useState("");
  const [busy, setBusy] = useState(false);
  const register = async () => {
    setBusy(true);
    setStatus("");
    try {
      await registerPasskey(name);
      setStatus("Passkey registered.");
    } catch (e) {
      setStatus(messageOf(e));
    } finally {
      setBusy(false);
    }
  };
  return (
    <Page eyebrow="identity" title="Passkeys" sub="Register an attestation-less passkey for this account.">
      <div className="panel">
        <label>
          Passkey name <input id="passkey-name" type="text" value={name} onChange={(e) => setName(e.target.value)} />
        </label>
        <div className="actions">
          <button className="primary" id="register-passkey" type="button" onClick={register} disabled={busy}>
            Register passkey
          </button>
          <span id="passkey-status" className="muted" role="status" aria-live="polite">
            {status}
          </span>
        </div>
      </div>
    </Page>
  );
}
