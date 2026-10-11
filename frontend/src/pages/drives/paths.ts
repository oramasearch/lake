/** Drives' pure helpers (lake#561, docs/ui/drives.md): drive paths and names, the upload plan for
 * a folder chosen or dropped with its subfolders, the move plan for items dragged onto a folder,
 * sorting, range selection and upload chunks. No React and no `@/` imports, so
 * `tests/drives.test.ts` runs them under Node directly. */

/* ------------------------------------------------------------------ paths */

/** A drive path never starts or ends with "/"; the drive's top is "". */
export function cleanPath(path: string): string {
  return path
    .split("/")
    .filter((part) => part && part !== ".")
    .join("/");
}

export function joinPath(folder: string, name: string): string {
  const a = cleanPath(folder);
  const b = cleanPath(name);
  return a && b ? `${a}/${b}` : a || b;
}

export function parentOf(path: string): string {
  const slash = path.lastIndexOf("/");
  return slash < 0 ? "" : path.slice(0, slash);
}

export function nameOf(path: string): string {
  const slash = path.lastIndexOf("/");
  return slash < 0 ? path : path.slice(slash + 1);
}

/** Every folder from the top down to `path` itself: "a/b/c" → ["", "a", "a/b", "a/b/c"]. */
export function ancestors(path: string): string[] {
  const parts = cleanPath(path).split("/").filter(Boolean);
  return ["", ...parts.map((_, index) => parts.slice(0, index + 1).join("/"))];
}

/** Whether `path` is `folder` or inside it. */
export function isWithin(path: string, folder: string): boolean {
  if (!folder) return true;
  return path === folder || path.startsWith(folder + "/");
}

/** A name a drive accepts for one file, folder or shortcut, as lake checks it: 1 to 255 bytes, no
 * "/" or "\\", no control character, not "." or "..", and not the reserved empty-folder marker. */
export function validName(name: string): boolean {
  if (!name || name === "." || name === ".." || name === ".lake-folder") return false;
  if (new TextEncoder().encode(name).length > 255) return false;
  for (const ch of name) {
    const code = ch.codePointAt(0)!;
    if (ch === "/" || ch === "\\" || code < 0x20 || code === 0x7f) return false;
  }
  return true;
}

/* ------------------------------------------------------------------ naming */

/** A repository name (a slug: lowercase letters, numbers, inner hyphens) from a drive's name, as
 * New drive suggests it for a repository drive. */
export function driveSlug(title: string): string {
  return title
    .normalize("NFKD")
    .replace(/[̀-ͯ]/g, "")
    .toLowerCase()
    .replace(/[^a-z0-9]+/g, "-")
    .replace(/^-+|-+$/g, "")
    .slice(0, 100)
    .replace(/-+$/g, "");
}

/* ------------------------------------------------------------------ entries */

export type EntryKind = "folder" | "file" | "shortcut";
export type SortKey = "name" | "size" | "modified";
export type Direction = "asc" | "desc";

/** Folders first, then by the key; names break ties. */
export function sortEntries<T extends { path: string; kind: EntryKind; size: number; modifiedAt?: string }>(entries: T[], sort: SortKey, direction: Direction): T[] {
  const sign = direction === "desc" ? -1 : 1;
  return [...entries].sort((a, b) => {
    if (a.kind !== b.kind) return a.kind === "folder" ? -1 : 1;
    let order = 0;
    if (sort === "size") order = a.size - b.size;
    else if (sort === "modified") order = (a.modifiedAt ?? "").localeCompare(b.modifiedAt ?? "");
    if (order === 0) order = nameOf(a.path).localeCompare(nameOf(b.path), undefined, { numeric: true, sensitivity: "base" });
    return order * sign;
  });
}

/** Shift-click: every path between the anchor and the target, in the order shown. Without an
 * anchor on screen, the target alone. */
export function rangeSelect(order: string[], anchor: string | null, target: string): string[] {
  const to = order.indexOf(target);
  const from = anchor === null ? -1 : order.indexOf(anchor);
  if (to < 0) return [];
  if (from < 0) return [target];
  const [lo, hi] = from <= to ? [from, to] : [to, from];
  return order.slice(lo, hi + 1);
}

/* ------------------------------------------------------------------ moving */

export type Move = { from: string; to: string };
export type Skipped = { path: string; reason: string };

/** Items dropped on (or moved to) a folder: each keeps its name in the target. An item already
 * there is left alone, and a folder never moves into itself. Of a folder and something inside it,
 * only the folder moves. */
export function planMoves(paths: string[], target: string): { moves: Move[]; skipped: Skipped[] } {
  const folder = cleanPath(target);
  const unique = [...new Set(paths.map(cleanPath))].filter(Boolean);
  const roots = unique.filter((path) => !unique.some((other) => other !== path && isWithin(path, other)));
  const moves: Move[] = [];
  const skipped: Skipped[] = [];
  const taken = new Set<string>();
  for (const path of roots) {
    if (parentOf(path) === folder) {
      skipped.push({ path, reason: "already in this folder" });
      continue;
    }
    if (isWithin(folder, path)) {
      skipped.push({ path, reason: "a folder cannot move into itself" });
      continue;
    }
    const to = joinPath(folder, nameOf(path));
    if (taken.has(to)) {
      skipped.push({ path, reason: "another item with this name is moving there" });
      continue;
    }
    taken.add(to);
    moves.push({ from: path, to });
  }
  return { moves, skipped };
}

/* ------------------------------------------------------------------ uploading */

/** One chosen or dropped file and where it goes, relative to the folder it is uploaded into:
 * "photo.jpg", or "Trip/Day 1/photo.jpg" for a folder chosen or dropped whole. */
export type UploadItem<F = unknown> = { relativePath: string; size: number; file: F };

export type UploadPlan<F = unknown> = {
  /** Folders to create first, parents before children, as full drive paths. */
  folders: string[];
  /** The files of each folder (a full drive path), in the order the folders were created. */
  batches: { folder: string; files: { name: string; size: number; file: F }[] }[];
  /** Files whose path a drive refuses. */
  refused: string[];
};

/** Group files by the folder they land in, under `base`. */
export function planUpload<F>(base: string, items: UploadItem<F>[]): UploadPlan<F> {
  const folders = new Set<string>();
  const byFolder = new Map<string, { name: string; size: number; file: F }[]>();
  const refused: string[] = [];
  for (const item of items) {
    const parts = item.relativePath.split("/").filter((part) => part && part !== ".");
    if (!parts.length || !parts.every(validName)) {
      refused.push(item.relativePath);
      continue;
    }
    const relativeFolder = parts.slice(0, -1).join("/");
    for (let i = 1; i < parts.length; i++) folders.add(joinPath(base, parts.slice(0, i).join("/")));
    const folder = joinPath(base, relativeFolder);
    const list = byFolder.get(folder) ?? [];
    list.push({ name: parts[parts.length - 1], size: item.size, file: item.file });
    byFolder.set(folder, list);
  }
  const depth = (path: string) => (path ? path.split("/").length : 0);
  const ordered = [...folders].sort((a, b) => depth(a) - depth(b) || a.localeCompare(b));
  const batchOrder = [...byFolder.keys()].sort((a, b) => depth(a) - depth(b) || a.localeCompare(b));
  return { folders: ordered, batches: batchOrder.map((folder) => ({ folder, files: byFolder.get(folder)! })), refused };
}

/** Base64 grows bytes by 4/3; with each file's name and JSON around it, an upload's body. */
export function encodedSize(files: { name: string; size: number }[]): number {
  return files.reduce((sum, file) => sum + Math.ceil(file.size / 3) * 4 + file.name.length + 32, 256);
}

/** Split one folder's files into requests that each fit `limit` (encoded). A file that alone is
 * larger is returned in `tooLarge`. */
export function chunkBySize<T extends { name: string; size: number }>(files: T[], limit: number): { chunks: T[][]; tooLarge: T[] } {
  const chunks: T[][] = [];
  const tooLarge: T[] = [];
  let current: T[] = [];
  for (const file of files) {
    if (encodedSize([file]) > limit) {
      tooLarge.push(file);
      continue;
    }
    if (current.length && encodedSize([...current, file]) > limit) {
      chunks.push(current);
      current = [];
    }
    current.push(file);
  }
  if (current.length) chunks.push(current);
  return { chunks, tooLarge };
}

/** The chunks one large file is sent in: each starts at a multiple of `block` and is at most
 * `limit` bytes (rounded down to whole blocks), the last one whatever is left. */
export function chunkRanges(size: number, block: number, limit: number): { offset: number; length: number }[] {
  const step = Math.max(block, Math.floor(limit / block) * block);
  const out: { offset: number; length: number }[] = [];
  for (let offset = 0; offset < size; offset += step) out.push({ offset, length: Math.min(step, size - offset) });
  return out;
}

/* ------------------------------------------------------------------ the folder tree */

/** The folder tree's open folders, kept as a set of paths: opening one also opens its ancestors,
 * so the current folder is always visible. */
export function expandTo(open: ReadonlySet<string>, path: string): Set<string> {
  const next = new Set(open);
  for (const folder of ancestors(path)) next.add(folder);
  return next;
}

/* ------------------------------------------------------------------ datasets */

/** The dataset format a drive file's name says (lake#593): csv for ".csv", jsonl for ".jsonl" or
 * ".ndjson", or null when it is neither, which "Load as dataset" does not offer. */
export function datasetFormat(name: string): "csv" | "jsonl" | null {
  const lower = name.toLowerCase();
  if (lower.endsWith(".csv")) return "csv";
  if (lower.endsWith(".jsonl") || lower.endsWith(".ndjson")) return "jsonl";
  return null;
}

/** A dataset name from a file's name, as SQL can query it: letters, digits and underscores,
 * starting with a letter or underscore, at most 63 characters ("Churn 2026.csv" → "churn_2026"). */
export function datasetName(fileName: string): string {
  const dot = fileName.lastIndexOf(".");
  const stem = (dot > 0 ? fileName.slice(0, dot) : fileName).toLowerCase();
  let name = stem.replace(/[^a-z0-9_]+/g, "_").replace(/^_+|_+$/g, "").replace(/_+/g, "_");
  if (!name) name = "dataset";
  if (/^[0-9]/.test(name)) name = `_${name}`;
  return name.slice(0, 63);
}
