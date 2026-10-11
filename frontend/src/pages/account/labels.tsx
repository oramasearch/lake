/** Words for the account pages' stored values. */
import { Badge } from "@/lib/ui";

/** A session's kind as words. */
export function sessionKind(kind: string | null | undefined): string {
  switch (kind) {
    case "session":
      return "Session";
    case "api_token":
      return "API token";
    case "static":
      return "Static";
    default: {
      const words = (kind ?? "").replace(/[_-]+/g, " ").trim();
      return words ? words.charAt(0).toUpperCase() + words.slice(1) : "—";
    }
  }
}

/** Active until revoked (state_badge). */
export function ActiveBadge({ revokedAt }: { revokedAt: string | null | undefined }) {
  return revokedAt ? <Badge tone="danger">Revoked</Badge> : <Badge tone="success">Active</Badge>;
}

export type SessionRow = {
  id: string;
  kind: string;
  name: string;
  scopes?: string[];
  expires_at?: string | null;
  revoked_at?: string | null;
  created_at?: string;
  /** Answered by the store for a static or never-expiring token, not declared in the schema. */
  reason?: string;
};

export type DeviceRow = { id: string; name: string; revoked_at?: string | null; created_at?: string };
