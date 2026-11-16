/** The terms of service and privacy policy that sign-up and sign-in link to.
 * The instance configures them (LAKE_TERMS_URL, LAKE_PRIVACY_URL, defaulting to Orama's published
 * pages) and `viewer.legal` reports them; an empty URL means the instance shows no link. The
 * server accepts only absolute http(s) URLs; `safeLegalUrl` checks again before rendering. */

export type LegalLinks = { terms_url?: string; privacy_url?: string };

/** A piece of the notice: plain text, or a link. */
export type LegalPart = { text: string } | { href: string; label: string };

export const TERMS_LABEL = "Terms of Service";
export const PRIVACY_LABEL = "Privacy Policy";

/** The URL when it is an absolute http or https URL, else "" (no link). */
export function safeLegalUrl(url: string | undefined): string {
  if (!url) return "";
  try {
    const parsed = new URL(url);
    return parsed.protocol === "https:" || parsed.protocol === "http:" ? url : "";
  } catch {
    return "";
  }
}

/** The sentence under the sign-up or sign-in form, as text and links, or null when the instance
 * links to neither document. */
export function legalNotice(mode: "sign-in" | "sign-up", legal: LegalLinks | undefined): LegalPart[] | null {
  const terms = safeLegalUrl(legal?.terms_url);
  const privacy = safeLegalUrl(legal?.privacy_url);
  if (!terms && !privacy) return null;
  const action = mode === "sign-up" ? "By creating an account" : "By signing in";
  const termsLink: LegalPart = { href: terms, label: TERMS_LABEL };
  const privacyLink: LegalPart = { href: privacy, label: PRIVACY_LABEL };
  if (terms && privacy) {
    return [{ text: `${action}, you agree to the ` }, termsLink, { text: " and acknowledge the " }, privacyLink, { text: "." }];
  }
  if (terms) return [{ text: `${action}, you agree to the ` }, termsLink, { text: "." }];
  return [{ text: "How we handle your data: " }, privacyLink, { text: "." }];
}
