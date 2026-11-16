// SPDX-License-Identifier: MIT

/** Sign in on a Lake whose only way in is the customer's identity provider (lake#1136,
 * `viewer.sign_in_sso`). One button: SAML goes to Lake's sign-on URL, OIDC through `start_sso_oidc`.
 * Lake's own password, passkey and social sign-in are refused by the server in that mode; the page
 * does not offer them. Operator sign-in stays, for the instance's administrators. */
import { useState } from "react";
import { Link, useLocation } from "react-router-dom";
import { act } from "@/lib/api";
import { Button, Notice, Page } from "@/lib/ui";
import { type SsoSignIn as Provider, ssoButtonLabel, ssoStart } from "@/lib/ssoSignIn";
import { AuthFrame } from "./AuthFrame";
import { leaveFor, messageOf, NO_WORKSPACE } from "./signIn";

export function SsoSignIn({ provider, mode }: { provider: Provider; mode: "sign-in" | "sign-up" }) {
  const location = useLocation();
  const params = new URLSearchParams(location.search);
  const returnTo = params.get("next") ?? params.get("return_to");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<unknown>(params.get("error"));
  const start = async () => {
    setBusy(true);
    setError(null);
    const how = ssoStart(provider, returnTo);
    if (how.kind === "redirect") {
      leaveFor(how.url);
      return;
    }
    try {
      const value = await act<{ authorization_url?: string }>(
        "start_sso_oidc",
        { provider_id: provider.provider_id, device_name: "browser" },
        NO_WORKSPACE,
      );
      leaveFor(value.authorization_url || "/sign-in");
    } catch (e) {
      setError(e);
      setBusy(false);
    }
  };
  return (
    <AuthFrame title={mode === "sign-up" ? "sign up" : "sign in"}>
      <Page eyebrow="identity" title="Sign In" sub="Sign in with your organization's account.">
        {error ? <Notice tone="danger">{typeof error === "string" ? error : messageOf(error)}</Notice> : null}
        <div>
          <Button variant="primary" loading={busy} onClick={start}>
            {ssoButtonLabel(provider)}
          </Button>
        </div>
        <p className="muted">
          <Link to="/login">Operator sign-in</Link>
        </p>
      </Page>
    </AuthFrame>
  );
}
