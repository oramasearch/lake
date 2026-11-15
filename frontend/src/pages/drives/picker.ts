// SPDX-License-Identifier: MIT

/** What `FolderPicker` shows and lets a person choose (lake#682, docs/ui/drives.md#the-picker): a
 * folder (Move), any item (a shortcut to an item), or only the items a caller accepts, such as a
 * model file (Train → Models, Bring one → A drive). A `search_drives` match becomes an item of its
 * drive here, so a search result is opened or chosen as a listed one is. Pure, so
 * `tests/drives.test.ts` runs it under Node. */
import { folderKey, type DriveMatch } from "./search.ts";
import type { Storage, Target } from "./targets.ts";

/** The fields of an item the picker reads (`Item` in storage.ts has these and more). */
export type PickerItem = {
  key: string;
  kind: "folder" | "file" | "shortcut";
  name: string;
  path: string;
  size: number;
  modifiedAt?: string;
  contentType?: string;
  status?: "ok" | "broken" | "unknown";
  reason?: string;
  target?: Target;
};

/** The drive a match is in, as `storageFor` takes it. */
export type PickerDrive = {
  id: string;
  name: string;
  storage: Storage;
  repository_id: string | null;
  store_id?: string | null;
  created_by: string;
  created_at: string;
};

/** What the picker picks: a folder to move into, or an item. `accept` narrows the items. */
export type PickMode = { kind: "folder" } | { kind: "item"; accept?: (item: PickerItem) => boolean };

/** Whether the row can be chosen (as against only opened). */
export function choosable(item: PickerItem, mode: PickMode): boolean {
  if (mode.kind === "folder") return item.kind === "folder";
  return mode.accept ? mode.accept(item) : true;
}

/** The rows a folder shows: folders always (to open them), and the items that can be chosen. */
export function pickerRows<T extends PickerItem>(items: T[], mode: PickMode): T[] {
  return items.filter((item) => item.kind === "folder" || choosable(item, mode));
}

/** A match of `search_drives` as an item of its drive: the key the drive's storage addresses it by
 * (its path in a repository or storage drive, its entry id in the others). */
export function matchItem(match: DriveMatch): PickerItem {
  return {
    key: folderKey(match),
    kind: match.kind === "drive" ? "folder" : match.kind,
    name: match.name,
    path: match.path,
    size: match.size ?? 0,
    modifiedAt: match.modified_at,
    contentType: match.content_type,
    status: match.status,
    reason: match.reason,
    target: match.target,
  };
}

/** The drive of a match, as the picker opens it. */
export function matchDrive(match: DriveMatch): PickerDrive {
  return { ...match.drive, created_by: "", created_at: "" };
}

/** The matches a search shows: drives and folders (to open them), and the items that can be
 * chosen. */
export function searchRows(matches: DriveMatch[], mode: PickMode): DriveMatch[] {
  return matches.filter((match) => match.kind === "drive" || match.kind === "folder" || choosable(matchItem(match), mode));
}

/** Whether a drive item can be brought as a model by `load_drive_model`: a file of a
 * workspace-files, repository or storage drive, or a shortcut that opens a file (an item of another
 * drive, or a path in a repository) and is not broken. */
export function bringable(item: PickerItem): boolean {
  if (item.kind === "file") return true;
  if (item.kind !== "shortcut" || item.status === "broken") return false;
  return item.target?.kind === "drive_entry" || item.target?.kind === "repository_path";
}

/** How a chosen item is named in a form: the drive, then its path. */
export function pickedLabel(drive: { name: string }, item: PickerItem): string {
  return `${drive.name} / ${item.path || item.name}`;
}
