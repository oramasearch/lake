/** How a drive keeps its files, behind one interface (lake#561, docs/ui/drives.md): the Drives
 * pages browse, upload, move, rename and delete through a `DriveStorage` and never ask which kind
 * it is, except for what it can do (`can`). Three implementations:
 *
 * - `RepositoryStorage`: a repository with history, through the drive_* capabilities; an item's
 *   key is its path, and every write is a change.
 * - `FilesStorage`: workspace files in the workspace Zetta, through list_drive_entries and the
 *   upload capabilities; an item's key is its id, and a move rewrites one record.
 * - `VirtualStorage`: the same records holding only folders and shortcuts.
 *
 * Every call goes through `/v1`, as the rest of the app does. */
import { act, read } from "@/lib/api";
import { bytesToBase64, displaySize } from "@/pages/repositories/forge";
import { chunkBySize, chunkRanges, joinPath, nameOf, parentOf, planUpload, type UploadItem } from "./paths";
import { cleanTarget, INLINE_TYPES, type Storage, type Target } from "./targets";

export type Drive = {
  id: string;
  name: string;
  storage: Storage;
  repository_id: string | null;
  created_by: string;
  created_at: string;
};

export type ItemKind = "folder" | "file" | "shortcut";

/** One row of a folder, whatever the drive keeps it in. `key` is what the storage addresses it by
 * (a path in a repository, an id elsewhere). */
export type Item = {
  key: string;
  kind: ItemKind;
  name: string;
  path: string;
  size: number;
  modifiedAt?: string;
  contentType?: string;
  sha256?: string | null;
  status?: "ok" | "broken" | "unknown";
  reason?: string;
  target?: Target;
};

export type Crumb = { key: string; name: string };

export type Listing = {
  /** The folder's own key ("" for the drive's top). */
  key: string;
  path: string;
  /** From the top down to this folder, the top excluded. */
  crumbs: Crumb[];
  items: Item[];
};

export type Progress = { sent: number; total: number; file: string };

export type UploadReport = { uploaded: number; refused: string[] };
export type MoveReport = { moved: number; skipped: { name: string; reason: string }[] };

export type Scope = { customerId: string; workspaceId: string };

export interface DriveStorage {
  readonly drive: Drive;
  readonly can: { upload: boolean; shortcuts: boolean; history: boolean; share: boolean };
  list(folder: string, signal?: AbortSignal): Promise<Listing>;
  createFolder(folder: string, name: string): Promise<void>;
  upload(folder: string, items: UploadItem<File>[], progress?: (p: Progress) => void): Promise<UploadReport>;
  rename(item: Item, name: string): Promise<void>;
  move(items: Item[], folder: string): Promise<MoveReport>;
  remove(item: Item): Promise<void>;
  createShortcut(folder: string, name: string, target: Target): Promise<void>;
  /** The URL that downloads a file, or null. */
  downloadHref(item: Item): string | null;
  /** The URL that shows a file in lake or the browser, or null when it only downloads. */
  viewHref(item: Item): string | null;
}

/** The REST body limit (64 MiB, base64 and JSON included). */
const MAX_BODY = 64 * 1024 * 1024;

const q = (values: Record<string, string>) =>
  Object.entries(values)
    .filter(([, v]) => v !== "")
    .map(([k, v]) => `${encodeURIComponent(k)}=${encodeURIComponent(v)}`)
    .join("&");

async function base64Of(blob: Blob): Promise<string> {
  return bytesToBase64(new Uint8Array(await blob.arrayBuffer()));
}

/* ------------------------------------------------------------------ repository */

type TreeEntry = { path: string; kind: string; size: number; modified_at?: string };

/** A repository drive: drive_browse, create_drive_folder, upload_drive_files, move_drive_path and
 * delete_drive_path on `main`. The repository capabilities are not workspace-scoped. */
export class RepositoryStorage implements DriveStorage {
  readonly can = { upload: true, shortcuts: false, history: true, share: true };
  private readonly repository: string;
  constructor(readonly drive: Drive) {
    this.repository = drive.repository_id ?? "";
  }

  async list(folder: string, signal?: AbortSignal): Promise<Listing> {
    const browse = await read<{ path: string; items: TreeEntry[] }>(
      "drive_browse",
      { repository_id: this.repository, ref: "main", path: folder },
      { workspace: null, signal },
    );
    const parts = folder ? folder.split("/") : [];
    return {
      key: folder,
      path: folder,
      crumbs: parts.map((name, i) => ({ key: parts.slice(0, i + 1).join("/"), name })),
      items: (browse.items ?? []).map((entry) => ({
        key: entry.path,
        kind: entry.kind === "tree" ? "folder" : "file",
        name: nameOf(entry.path),
        path: entry.path,
        size: entry.size ?? 0,
        modifiedAt: entry.modified_at,
      })),
    };
  }

  async createFolder(folder: string, name: string) {
    await act("create_drive_folder", { repository_id: this.repository, ref: "main", path: folder, name }, { workspace: null });
  }

  /** Folders first (each a change, a folder already there is fine), then the files of each folder
   * in as few changes as the body limit allows. */
  async upload(folder: string, items: UploadItem<File>[], progress?: (p: Progress) => void): Promise<UploadReport> {
    const plan = planUpload(folder, items);
    const refused = [...plan.refused];
    const existing = new Set<string>();
    for (const path of plan.folders) {
      if (existing.has(path)) continue;
      try {
        await this.createFolder(parentOf(path), nameOf(path));
      } catch (error) {
        // A folder that is already there is the folder to upload into.
        if ((error as { code?: string }).code !== "conflict") throw error;
      }
      existing.add(path);
    }
    const total = items.reduce((sum, item) => sum + item.size, 0);
    let sent = 0;
    let uploaded = 0;
    for (const batch of plan.batches) {
      const { chunks, tooLarge } = chunkBySize(batch.files, MAX_BODY);
      for (const file of tooLarge) refused.push(`${joinPath(batch.folder, file.name)} (${displaySize(file.size)}: larger than one request can carry)`);
      for (const chunk of chunks) {
        progress?.({ sent, total, file: chunk[0].name });
        const files = await Promise.all(chunk.map(async (f) => ({ name: f.name, content: await base64Of(f.file) })));
        await act("upload_drive_files", { repository_id: this.repository, ref: "main", path: batch.folder, files }, { workspace: null });
        uploaded += chunk.length;
        sent += chunk.reduce((sum, f) => sum + f.size, 0);
        progress?.({ sent, total, file: chunk[chunk.length - 1].name });
      }
    }
    return { uploaded, refused };
  }

  async rename(item: Item, name: string) {
    await act("move_drive_path", { repository_id: this.repository, ref: "main", path: item.key, destination: joinPath(parentOf(item.key), name) }, { workspace: null });
  }

  async move(items: Item[], folder: string): Promise<MoveReport> {
    const skipped: MoveReport["skipped"] = [];
    let moved = 0;
    for (const item of items) {
      if (parentOf(item.key) === folder) {
        skipped.push({ name: item.name, reason: "already in this folder" });
        continue;
      }
      if (folder === item.key || folder.startsWith(item.key + "/")) {
        skipped.push({ name: item.name, reason: "a folder cannot move into itself" });
        continue;
      }
      await act("move_drive_path", { repository_id: this.repository, ref: "main", path: item.key, destination: joinPath(folder, item.name) }, { workspace: null });
      moved += 1;
    }
    return { moved, skipped };
  }

  async remove(item: Item) {
    await act("delete_drive_path", { repository_id: this.repository, ref: "main", path: item.key }, { workspace: null });
  }

  async createShortcut(): Promise<void> {
    throw new Error("A repository drive holds no shortcuts.");
  }

  downloadHref(item: Item) {
    return item.kind === "file" ? `/drive/download?${q({ repository_id: this.repository, ref: "main", path: item.key })}` : null;
  }

  viewHref(item: Item) {
    return item.kind === "file" ? `/drive/preview?${q({ repository_id: this.repository, ref: "main", path: item.key })}` : null;
  }
}

/* ------------------------------------------------------------------ workspace files */

type Entry = {
  id: string;
  kind: ItemKind;
  name: string;
  path: string;
  size?: number;
  content_type?: string;
  sha256?: string | null;
  modified_at?: string;
  status?: Item["status"];
  reason?: string;
  target?: Target;
};

type EntryListing = { folder_id: string | null; path: string; ancestors: { id: string; name: string }[]; items: Entry[] };

/** Files at most this big go several to a call (upload_drive_entries); larger ones are sent in
 * chunks (begin_drive_upload, append_drive_upload, finish_drive_upload). */
const SMALL_FILE = 4 * 1024 * 1024;
const SMALL_BATCH = 12 * 1024 * 1024;
const BLOCK = 1024 * 1024;
const CHUNK = 8 * 1024 * 1024;

function itemOf(entry: Entry): Item {
  return {
    key: entry.id,
    kind: entry.kind,
    name: entry.name,
    path: entry.path,
    size: entry.size ?? 0,
    modifiedAt: entry.modified_at,
    contentType: entry.content_type,
    sha256: entry.sha256,
    status: entry.status,
    reason: entry.reason,
    target: entry.target,
  };
}

/** A workspace-files drive: records in the workspace Zetta, bytes in its `lake_files` bucket. */
export class FilesStorage implements DriveStorage {
  readonly can: DriveStorage["can"] = { upload: true, shortcuts: true, history: false, share: false };
  constructor(
    readonly drive: Drive,
    protected readonly scope: Scope,
  ) {}

  protected input(extra: Record<string, unknown> = {}) {
    return { customer_id: this.scope.customerId, workspace_id: this.scope.workspaceId, drive_id: this.drive.id, ...extra };
  }

  async list(folder: string, signal?: AbortSignal): Promise<Listing> {
    const listing = await read<EntryListing>(
      "list_drive_entries",
      { customer_id: this.scope.customerId, workspace_id: this.scope.workspaceId, drive_id: this.drive.id, folder_id: folder },
      { signal },
    );
    return {
      key: listing.folder_id ?? "",
      path: listing.path,
      crumbs: listing.ancestors.map((a) => ({ key: a.id, name: a.name })),
      items: listing.items.map(itemOf),
    };
  }

  async createFolder(folder: string, name: string) {
    await act("create_drive_folder_entry", this.input({ parent_id: folder, path: name }));
  }

  /** Each file at its relative path (its folders made by lake). Small files go several to a call;
   * a large one in 8 MiB chunks, so a file of any size never needs one large body. */
  async upload(folder: string, items: UploadItem<File>[], progress?: (p: Progress) => void): Promise<UploadReport> {
    const total = items.reduce((sum, item) => sum + item.size, 0);
    let sent = 0;
    let uploaded = 0;
    const refused: string[] = [];
    const small = items.filter((item) => item.size <= SMALL_FILE);
    const large = items.filter((item) => item.size > SMALL_FILE);
    let batch: UploadItem<File>[] = [];
    let batchBytes = 0;
    const flush = async () => {
      if (!batch.length) return;
      progress?.({ sent, total, file: batch[0].relativePath });
      const files = await Promise.all(batch.map(async (item) => ({ path: item.relativePath, content: await base64Of(item.file), ...(item.file.type ? { content_type: item.file.type } : {}) })));
      await act("upload_drive_entries", this.input({ parent_id: folder, files }));
      uploaded += batch.length;
      sent += batchBytes;
      progress?.({ sent, total, file: batch[batch.length - 1].relativePath });
      batch = [];
      batchBytes = 0;
    };
    for (const item of small) {
      if (batch.length && batchBytes + item.size > SMALL_BATCH) await flush();
      batch.push(item);
      batchBytes += item.size;
    }
    await flush();
    for (const item of large) {
      const begun = await act<{ entry: Entry; block_size: number; max_chunk_bytes: number }>(
        "begin_drive_upload",
        this.input({ parent_id: folder, path: item.relativePath, size: item.size, ...(item.file.type ? { content_type: item.file.type } : {}) }),
      );
      const id = begun.entry.id;
      for (const { offset, length } of chunkRanges(item.size, begun.block_size || BLOCK, Math.min(begun.max_chunk_bytes || CHUNK, CHUNK))) {
        progress?.({ sent: sent + offset, total, file: item.relativePath });
        const content = await base64Of(item.file.slice(offset, offset + length));
        await act("append_drive_upload", this.input({ id, offset, content }));
      }
      await act("finish_drive_upload", this.input({ id }));
      uploaded += 1;
      sent += item.size;
      progress?.({ sent, total, file: item.relativePath });
    }
    return { uploaded, refused };
  }

  async rename(item: Item, name: string) {
    await act("move_drive_entry", this.input({ id: item.key, name }));
  }

  async move(items: Item[], folder: string): Promise<MoveReport> {
    const skipped: MoveReport["skipped"] = [];
    let moved = 0;
    for (const item of items) {
      if (item.key === folder) {
        skipped.push({ name: item.name, reason: "a folder cannot move into itself" });
        continue;
      }
      try {
        await act("move_drive_entry", this.input({ id: item.key, parent_id: folder }));
        moved += 1;
      } catch (error) {
        skipped.push({ name: item.name, reason: (error as Error).message });
      }
    }
    return { moved, skipped };
  }

  async remove(item: Item) {
    await act("delete_drive_entry", this.input({ id: item.key }));
  }

  async createShortcut(folder: string, name: string, target: Target) {
    await act("create_drive_shortcut", this.input({ parent_id: folder, name, target: cleanTarget(target) }));
  }

  private fileHref(item: Item, inline: boolean) {
    return `/drive/files/download?${q({
      customer_id: this.scope.customerId,
      workspace_id: this.scope.workspaceId,
      drive_id: this.drive.id,
      id: item.key,
      ...(inline ? { inline: "1" } : {}),
    })}`;
  }

  downloadHref(item: Item) {
    return item.kind === "file" ? this.fileHref(item, false) : null;
  }

  viewHref(item: Item) {
    return item.kind === "file" && item.contentType && INLINE_TYPES.has(item.contentType) ? this.fileHref(item, true) : null;
  }
}

/** A virtual drive: folders and shortcuts only. */
export class VirtualStorage extends FilesStorage {
  readonly can: DriveStorage["can"] = { upload: false, shortcuts: true, history: false, share: false };

  async upload(): Promise<UploadReport> {
    throw new Error("A virtual drive holds folders and shortcuts only; upload into a workspace-files drive.");
  }
}

export function storageFor(drive: Drive, scope: Scope): DriveStorage {
  if (drive.storage === "repository") return new RepositoryStorage(drive);
  if (drive.storage === "virtual") return new VirtualStorage(drive, scope);
  return new FilesStorage(drive, scope);
}
