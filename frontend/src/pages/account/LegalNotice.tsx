/** The terms and privacy sentence under every sign-up and sign-in form, Clerk's and lake's own
 * (liquid-bridge#15). The links come from `viewer.legal` and open in a new tab, so a half-filled
 * form is kept. Renders nothing when the instance links to neither document. */
import { useSession } from "@/lib/session";
import { legalNotice } from "@/lib/legalLinks";

export function LegalNotice({ mode }: { mode: "sign-in" | "sign-up" }) {
  const { viewer } = useSession();
  const parts = legalNotice(mode, viewer.legal);
  if (!parts) return null;
  return (
    <p className="muted auth-legal" data-testid="legal-notice">
      {parts.map((part, i) =>
        "href" in part ? (
          <a key={i} href={part.href} target="_blank" rel="noopener noreferrer">
            {part.label}
          </a>
        ) : (
          <span key={i}>{part.text}</span>
        ),
      )}
    </p>
  );
}
