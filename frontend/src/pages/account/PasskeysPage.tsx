/** Passkeys (`/passkeys`): register an attestation-less passkey for this account. Registered
 * passkeys appear as devices on Sessions. */
import { useState } from "react";
import { Fingerprint, MonitorSmartphone } from "lucide-react";
import { Banner, Button, Card, Page, Split } from "@/lib/ui";
import { useTrail } from "@/lib/trail";
import { messageOf } from "./signIn";
import { registerPasskey } from "./webauthn";

export default function PasskeysPage() {
  useTrail([{ label: "Account", to: "/settings" }, { label: "Passkeys" }]);
  const [name, setName] = useState("passkey");
  const [status, setStatus] = useState("");
  const [ok, setOk] = useState(false);
  const [busy, setBusy] = useState(false);
  const register = async () => {
    setBusy(true);
    setStatus("");
    setOk(false);
    try {
      await registerPasskey(name);
      setStatus("Passkey registered.");
      setOk(true);
    } catch (e) {
      setStatus(messageOf(e));
    } finally {
      setBusy(false);
    }
  };
  return (
    <Page eyebrow="Account" title="Passkeys" sub="Register an attestation-less passkey for this account.">
      <Split
        side={
          <Card label="Where they appear">
            <p className="mt-0 text-[13px] text-ink-2">Registered passkeys appear as devices on Sessions, where you can revoke them.</p>
            <Button to="/sessions">
              <MonitorSmartphone size={14} aria-hidden /> Sessions and devices
            </Button>
          </Card>
        }
      >
        <Card label="New passkey" title="Register a passkey">
          <form
            className="stack"
            onSubmit={(e) => {
              e.preventDefault();
              void register();
            }}
          >
            <div className="field">
              <label htmlFor="passkey-name">Passkey name</label>
              <input id="passkey-name" type="text" value={name} onChange={(e) => setName(e.target.value)} />
              <small className="muted">Name it after the device or key, so you can tell it apart later.</small>
            </div>
            <div className="flex flex-wrap items-center gap-3">
              <Button type="submit" variant="primary" loading={busy}>
                <Fingerprint size={14} aria-hidden /> Register passkey
              </Button>
            </div>
          </form>
          <div id="passkey-status" aria-live="polite" className="mt-3">
            {status && (
              <Banner tone={ok ? "ok" : "warn"}>
                {status}
              </Banner>
            )}
          </div>
        </Card>
      </Split>
    </Page>
  );
}
