/** What `search_drives` answers and where each match takes the person (lake#561,
 * docs/api/drives.md#search): Data → Search's Drives place and a drive's own search box both use
 * it. Pure, so `tests/drives.test.ts` runs it under Node. */
import { INLINE_TYPES, storageLabel, targetHref, type Storage, type Target } from "./targets.ts";

export type DriveMatch = {
  drive: { id: string; name: string; storage: Storage; repository_id: string | null; store_id?: string | null };
  kind: "drive" | "folder" | "file" | "shortcut";
  /** The entry's id in a workspace-files or virtual drive, its path in a store drive; null for a
   * drive or a repository path. */
  id: string | null;
  parent_id: string | null;
  name: string;
  /** From the drive's top, the item's own name included ("" for a drive). */
  path: string;
  /** The path of the folder that holds it ("" for the top). */
  folder: string;
  size?: number;
  content_type?: string;
  modified_at?: string;
  target?: Target;
  status?: "ok" | "broken" | "unknown";
  reason?: string;
};

export type SearchDrivesResult = {
  items: DriveMatch[];
  truncated: boolean;
  skipped: { drive: DriveMatch["drive"]; reason: string }[];
};

/** Where a match opens. `to` is a page of the app (a workspace page without its `/w/<id>` prefix
 * when `workspace`); `href` is a URL the browser opens itself (a file, or a web address). */
export type MatchLink = { to: string; workspace: boolean } | { href: string; newTab: boolean; download: boolean };

const q = (values: Record<string, string | null | undefined>) =>
  Object.entries(values)
    .filter(([, v]) => v)
    .map(([k, v]) => `${encodeURIComponent(k)}=${encodeURIComponent(v!)}`)
    .join("&");

/** The key the Drive page opens a folder by: its path in a repository drive, its id elsewhere. */
export function folderKey(match: DriveMatch): string {
  return match.drive.storage === "repository" ? match.path : (match.id ?? "");
}

/** The key of the folder that holds the match, in the Drive page's terms. */
export function parentKey(match: DriveMatch): string {
  return match.drive.storage === "repository" ? match.folder : (match.parent_id ?? "");
}

export function matchLink(match: DriveMatch, scope: { customerId: string; workspaceId: string }): MatchLink {
  const drive = (extra: Record<string, string>) => ({ to: `/drives/drive?${q({ id: match.drive.id, ...extra })}`, workspace: true });
  switch (match.kind) {
    case "drive":
      return drive({});
    case "folder":
      return drive({ folder: folderKey(match) });
    case "file": {
      if (match.drive.storage === "repository") {
        return { to: `/drive/preview?${q({ repository_id: match.drive.repository_id, ref: "main", path: match.path })}`, workspace: false };
      }
      const inline = !!match.content_type && INLINE_TYPES.has(match.content_type);
      return {
        href: `/drive/files/download?${q({ customer_id: scope.customerId, workspace_id: scope.workspaceId, drive_id: match.drive.id, id: match.id, inline: inline ? "1" : "" })}`,
        newTab: inline,
        download: !inline,
      };
    }
    case "shortcut": {
      const target = match.status === "broken" ? null : targetHref(match.target);
      // A broken shortcut (or one lake cannot open) shows where it is, selected in its folder.
      if (!target) return drive({ focus: match.id ?? "" });
      return target.external ? { href: target.href, newTab: true, download: false } : { to: target.href, workspace: target.workspace };
    }
  }
}

/** The muted line under a match: its drive, then its folder's path. */
export function matchWhere(match: DriveMatch): string {
  if (match.kind === "drive") return `${storageLabel(match.drive.storage)} drive`;
  return [match.drive.name, ...(match.folder ? match.folder.split("/") : [])].join(" / ");
}
