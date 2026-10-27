/** Editing a repository file in the console (lake#1239, docs/ui/drive.md#editing-a-file): which
 * files the editor may open, the exact bytes it commits, the conflict it refuses, and the Apps a
 * file belongs to. Pure, so `npm test` covers it (tests/repository-edit.test.ts). */

/** The largest file the console edits in place; larger ones are downloaded and pushed. */
export const EDIT_LIMIT_BYTES = 1024 * 1024;

/** How the file ends its lines. A browser's textarea hands back LF only, so a CRLF file is
 * rewritten with CRLF on save, and a file that mixes them cannot be kept exact. */
export type LineEnding = "\n" | "\r\n";

export type EditableFile = { ok: true; text: string; eol: LineEnding } | { ok: false; reason: string };

/** base64 → bytes, without atob's Latin-1 string surviving anywhere. */
export function base64ToBytes(value: string): Uint8Array {
  const binary = atob(value);
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i += 1) bytes[i] = binary.charCodeAt(i);
  return bytes;
}

export function bytesToBase64(bytes: Uint8Array): string {
  let binary = "";
  const chunk = 0x8000;
  for (let i = 0; i < bytes.length; i += chunk) binary += String.fromCharCode(...bytes.subarray(i, i + chunk));
  return btoa(binary);
}

/** Whether a file read through `repository_file` can be edited as text, and its text. Read from
 * `content_base64`, never `contents` (which writes bytes outside ASCII as `\xNN`, lake#1238). The
 * text is exactly the file: strict UTF-8 (no replacement characters), its byte-order mark kept,
 * and one line ending throughout. */
export function editableFile(contentBase64: string | undefined, size: number): EditableFile {
  if (typeof contentBase64 !== "string") return { ok: false, reason: "Lake did not send this file's bytes." };
  if (size > EDIT_LIMIT_BYTES) return { ok: false, reason: "This file is larger than 1 MiB. Download it, change it and push it." };
  let bytes: Uint8Array;
  try {
    bytes = base64ToBytes(contentBase64);
  } catch {
    return { ok: false, reason: "Lake sent this file's bytes in a form the editor cannot read." };
  }
  if (bytes.includes(0)) return { ok: false, reason: "This is a binary file. Download it to change it." };
  let text: string;
  try {
    text = new TextDecoder("utf-8", { fatal: true, ignoreBOM: true }).decode(bytes);
  } catch {
    return { ok: false, reason: "This file is not UTF-8 text. Download it to change it." };
  }
  const crlf = (text.match(/\r\n/g) ?? []).length;
  const lf = (text.match(/\n/g) ?? []).length;
  const cr = (text.match(/\r/g) ?? []).length;
  if (cr !== crlf || (crlf > 0 && crlf !== lf)) {
    return { ok: false, reason: "This file mixes line endings, which the editor cannot keep exactly. Download it to change it." };
  }
  return { ok: true, text: crlf > 0 ? text.replace(/\r\n/g, "\n") : text, eol: crlf > 0 ? "\r\n" : "\n" };
}

/** The bytes the editor's text commits: UTF-8, with the file's own line ending. */
export function textBytes(text: string, eol: LineEnding): Uint8Array {
  const normalized = text.replace(/\r\n?/g, "\n");
  return new TextEncoder().encode(eol === "\r\n" ? normalized.replace(/\n/g, "\r\n") : normalized);
}

/** `commit_repository_files`' input for one edited file: its exact bytes, and the blob it held
 * when it was opened as `expected_blob`, so Lake refuses the commit with `conflict` when the file
 * changed on main since (never an overwrite). Edits are committed to main, the only branch Lake
 * writes. */
export function commitInput(repositoryId: string, path: string, text: string, eol: LineEnding, openedBlob: string, message: string) {
  return {
    id: repositoryId,
    revision: "main",
    message: message.trim(),
    operations: [{ type: "file", path, content: bytesToBase64(textBytes(text, eol)), expected_blob: openedBlob }],
  };
}

/** The message a commit gets when the person writes none. */
export function defaultMessage(path: string): string {
  return `Edit ${path.split("/").pop() || path}`;
}

/** Whether a failed commit is the stale-file conflict (as opposed to any other refusal). */
export function isConflict(error: unknown): boolean {
  return !!error && typeof error === "object" && (error as { code?: unknown }).code === "conflict";
}

/** Why the Edit button is not offered, or null when it is. */
export function editRefusal(options: { signedIn: boolean; canWrite: boolean; checking: boolean; refName: string; format: string; size: number }): string | null {
  if (!options.signedIn) return "Sign in with write access on this repository to edit its files.";
  if (options.checking) return "Checking your access…";
  if (!options.canWrite) return "You can read this file. Editing it needs write access on the repository.";
  if (options.refName !== "main") return "Edits are committed to main. Open this file on main to edit it.";
  if (!["text", "markdown", "csv", "json"].includes(options.format)) return "Only text files can be edited here. Download this file to change it.";
  if (options.size > EDIT_LIMIT_BYTES) return "This file is larger than 1 MiB. Download it, change it and push it.";
  return null;
}

export type AppSource = { id: string; name: string; mode?: string; source?: { repository?: string; path?: string; entrypoint?: string; revision?: string } | null };

/** Whether `path` is inside an App's source folder (`.` or empty: the whole repository). */
export function inSourcePath(sourcePath: string | undefined, path: string): boolean {
  const folder = (sourcePath ?? "").replace(/^\.?\/*/, "").replace(/\/+$/, "");
  return folder === "" || path === folder || path.startsWith(`${folder}/`);
}

/** The runtime Apps deployed from this repository whose source folder holds `path`: the ones
 * *Commit and redeploy* redeploys at the new revision. */
export function appsForFile(apps: AppSource[], repositoryId: string, path: string): AppSource[] {
  return apps.filter((app) => app.mode === "runtime" && app.source?.repository === repositoryId && inSourcePath(app.source?.path, path));
}

/** `deploy_app`'s input that redeploys an App from its source at `revision`. */
export function redeployInput(app: AppSource, customerId: string, workspaceId: string, revision: string) {
  return {
    customer_id: customerId,
    workspace_id: workspaceId,
    app_id: app.id,
    source: { repository: app.source?.repository ?? "", revision, path: app.source?.path ?? "", entrypoint: app.source?.entrypoint ?? "" },
  };
}

/** Where a rollout stands from `get_app`: still going, Ready at the deployed generation, or failed. */
export function rolloutState(app: { phase?: unknown; ready?: unknown; generation?: unknown; observed_generation?: unknown } | undefined, generation: number): "rolling" | "ready" | "failed" {
  if (!app) return "rolling";
  const observed = Number(app.observed_generation ?? 0);
  if (observed < generation) return "rolling";
  const phase = String(app.phase ?? "");
  if (phase === "Failed" || phase === "Degraded") return "failed";
  return app.ready === true || phase === "Running" || phase === "Ready" ? "ready" : "rolling";
}

export type DiffLine = { kind: "same" | "added" | "removed"; text: string };

/** A line diff from `before` to `after` (longest common subsequence), for the conflict view. Files
 * the editor opens are at most 1 MiB; past 4,000 lines a side it shows the two whole, removed then
 * added, rather than spend quadratic memory. */
export function lineDiff(before: string, after: string): DiffLine[] {
  const a = before.split("\n");
  const b = after.split("\n");
  if (a.length > 4000 || b.length > 4000) {
    return [...a.map((text) => ({ kind: "removed" as const, text })), ...b.map((text) => ({ kind: "added" as const, text }))];
  }
  const rows = a.length + 1;
  const cols = b.length + 1;
  const lengths = new Uint32Array(rows * cols);
  for (let i = a.length - 1; i >= 0; i -= 1) {
    for (let j = b.length - 1; j >= 0; j -= 1) {
      lengths[i * cols + j] = a[i] === b[j] ? lengths[(i + 1) * cols + j + 1] + 1 : Math.max(lengths[(i + 1) * cols + j], lengths[i * cols + j + 1]);
    }
  }
  const out: DiffLine[] = [];
  let i = 0;
  let j = 0;
  while (i < a.length && j < b.length) {
    if (a[i] === b[j]) {
      out.push({ kind: "same", text: a[i] });
      i += 1;
      j += 1;
    } else if (lengths[(i + 1) * cols + j] >= lengths[i * cols + j + 1]) {
      out.push({ kind: "removed", text: a[i] });
      i += 1;
    } else {
      out.push({ kind: "added", text: b[j] });
      j += 1;
    }
  }
  while (i < a.length) out.push({ kind: "removed", text: a[i++] });
  while (j < b.length) out.push({ kind: "added", text: b[j++] });
  return out;
}
