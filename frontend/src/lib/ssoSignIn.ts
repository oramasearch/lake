// SPDX-License-Identifier: MIT

/** SSO-only sign-in (lake#1136, docs/api/sso.md): a white-labelled Lake whose only way in is the
 * customer's own identity provider (LAKE_SIGN_IN_SSO_PROVIDER). The `viewer` read names it; the
 * sign-in and sign-up pages offer it alone. */

/** What `viewer.sign_in_sso` carries: the provider's id, protocol (`saml` or `oidc`) and name. */
export type SsoSignIn = { provider_id: string; protocol: string; name: string };

/** How sign-in starts: SAML opens Lake's sign-on URL, which records the request and redirects to
 * the identity provider; OIDC calls `start_sso_oidc`, which answers the provider's authorization
 * URL and sets the flow's browser cookie. `returnTo` is kept only when it is a same-origin path. */
export function ssoStart(provider: SsoSignIn, returnTo: string | null): { kind: "redirect"; url: string } | { kind: "oidc" } {
  if (provider.protocol === "oidc") return { kind: "oidc" };
  const base = `/v1/saml/${encodeURIComponent(provider.provider_id)}/start`;
  const kept = returnTo && returnTo.startsWith("/") && !returnTo.startsWith("//") && !returnTo.startsWith("/\\") ? returnTo : null;
  return { kind: "redirect", url: kept ? `${base}?return_to=${encodeURIComponent(kept)}` : base };
}

/** The button's words: the provider's name when it has one, never a product name. */
export function ssoButtonLabel(provider: SsoSignIn): string {
  const name = provider.name.trim();
  return name ? `Continue with ${name}` : "Continue with your organization";
}
