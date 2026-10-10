/** What the Drive sharing and public share pages share: link state and wording
 * (`share_link_state`, `share_link_actions`), the bearer-link addresses and the share error. */
import { useSession } from "@/lib/session";
import { useTrail } from "@/lib/trail";
import type { AccessEntry, PolicyObject, Repository } from "./forge";

export type ShareLink = { id: string; role: string; expires_at: string; revoked_at: string | null; active: boolean; workspace_id: string };
export type EffectiveAccess = { repository: Repository; object: PolicyObject; path: string; workspace_id: string; items: AccessEntry[]; links: ShareLink[] };
/** `share_link_state`. */
export function linkState(link: ShareLink): string {
  if (link.revoked_at) return "revoked";
  return link.active ? "active" : "expired";
}

/** `share_link_actions`. */
export function linkActions(role: string): string {
  return role === "write" ? "Read; write scope is available to capability-aware clients" : "Browse and download";
}

/** `shared_drive_url`. */
export function sharedUrl(token: string, ref: string, path: string, action = ""): string {
  return `/share/${encodeURIComponent(token)}${action ? `/${action}` : ""}?ref=${encodeURIComponent(ref)}&path=${encodeURIComponent(path)}`;
}

/** The share pages' trail: "Shared with Me › share link" for a signed-in viewer, else the link. */
export function useShareTrail(title: string) {
  const { viewer } = useSession();
  useTrail(viewer.signed_in ? [{ label: "Shared with Me", to: "/shared-with-me" }, { label: "share link" }] : [{ label: "share link" }], title);
}

/** `serve_drive_share_error`: a bad, expired or revoked link, or a path outside it. */
export function ShareError({ title, message }: { title: string; message: string }) {
  return (
    <div className="page-container">
      <p className="eyebrow">shared drive folder</p>
      <h1>{title}</h1>
      <p className="error">{message}</p>
    </div>
  );
}

