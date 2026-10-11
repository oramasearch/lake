/** What a drive is and what a shortcut points at (lake#561, docs/ui/drives.md): the words a page
 * shows and the page each shortcut opens. Pure, so `tests/drives.test.ts` runs it under Node. */

export type Storage = "repository" | "files" | "virtual" | "store";

export const STORAGE: { id: Storage; label: string; short: string; tagline: string; detail: string }[] = [
  {
    id: "files",
    label: "Workspace files",
    short: "Files",
    tagline: "Encrypted files in this workspace",
    detail: "Files and folders kept in this workspace, encrypted. Moving or renaming never copies them. No version history.",
  },
  {
    id: "repository",
    label: "Repository",
    short: "Repository",
    tagline: "Files with history and sharing",
    detail: "A repository with history: every upload, move and delete is a change you can compare and restore. Shareable by folder.",
  },
  {
    id: "virtual",
    label: "Virtual",
    short: "Virtual",
    tagline: "Shortcuts to what you already have",
    detail: "Folders of shortcuts to things that live elsewhere: repositories, files, datasets, transcripts, projects, agents, models and links.",
  },
  {
    id: "store",
    label: "Storage",
    short: "Storage",
    tagline: "A bucket or filesystem, browsed in place",
    detail: "One of this workspace's buckets or filesystems, opened as a drive. Nothing is copied; deleting the drive never deletes the storage.",
  },
];

export function storageLabel(storage: string | null | undefined): string {
  return STORAGE.find((s) => s.id === storage)?.label ?? "Drive";
}

export type TargetKind = "repository" | "repository_path" | "drive_entry" | "dataset" | "transcript" | "project" | "agent" | "model" | "url";

export type Target = {
  kind: TargetKind;
  repository_id?: string;
  path?: string;
  ref?: string;
  drive_id?: string;
  entry_id?: string;
  id?: string;
  name?: string;
  url?: string;
};

export const TARGETS: { kind: TargetKind; label: string; help: string }[] = [
  { kind: "repository", label: "Repository", help: "A repository of this workspace." },
  { kind: "repository_path", label: "File or folder in a repository", help: "A path in a repository, such as reports/2026/q3.pdf." },
  { kind: "drive_entry", label: "Item in another drive", help: "A file, folder or shortcut of a workspace-files or virtual drive." },
  { kind: "dataset", label: "Dataset", help: "A dataset of this workspace, as <database>/<dataset>." },
  { kind: "transcript", label: "Transcript", help: "A transcript of this workspace." },
  { kind: "project", label: "Project", help: "A Design project, by its name." },
  { kind: "agent", label: "Agent", help: "An agent of this workspace, by its id." },
  { kind: "model", label: "Model", help: "A model, by its id." },
  { kind: "url", label: "Web address", help: "Any http or https address." },
];

export function targetLabel(kind: string | undefined): string {
  return TARGETS.find((t) => t.kind === kind)?.label ?? "Shortcut";
}

/** The fields a shortcut of `kind` needs, as lake checks them; the first one missing, or null. */
export function targetProblem(target: Target): string | null {
  const need = (field: keyof Target, words: string) => (!target[field] || !String(target[field]).trim() ? words : null);
  switch (target.kind) {
    case "repository":
      return need("repository_id", "Choose a repository.");
    case "repository_path":
      return need("repository_id", "Choose a repository.") ?? need("path", "Name the path in the repository.");
    case "drive_entry":
      return need("drive_id", "Choose a drive.") ?? need("entry_id", "Choose an item.");
    case "dataset":
    case "transcript":
    case "agent":
    case "model":
      return need("id", "Name what the shortcut opens.");
    case "project":
      return need("name", "Name the project.");
    case "url": {
      const url = (target.url ?? "").trim();
      if (!/^https?:\/\/\S+$/.test(url) || /["<>]/.test(url)) return "A web address starts with http:// or https://.";
      return null;
    }
    default:
      return "Choose what the shortcut opens.";
  }
}

/** Only the fields of the kind, trimmed: what create_drive_shortcut is sent. */
export function cleanTarget(target: Target): Target {
  const keep: Record<TargetKind, (keyof Target)[]> = {
    repository: ["repository_id"],
    repository_path: ["repository_id", "path", "ref"],
    drive_entry: ["drive_id", "entry_id"],
    dataset: ["id"],
    transcript: ["id"],
    agent: ["id"],
    model: ["id"],
    project: ["name"],
    url: ["url"],
  };
  const out: Target = { kind: target.kind };
  for (const field of keep[target.kind] ?? []) {
    const value = target[field];
    if (typeof value !== "string" || !value.trim()) continue;
    // A repository path never starts or ends with "/".
    (out as Record<string, string>)[field] = field === "path" ? value.trim().replace(/^\/+|\/+$/g, "") : value.trim();
  }
  return out;
}

const q = (values: Record<string, string | undefined>) =>
  Object.entries(values)
    .filter(([, v]) => v)
    .map(([k, v]) => `${encodeURIComponent(k)}=${encodeURIComponent(v!)}`)
    .join("&");

/** Where a shortcut takes the person: a page path (workspace pages without their `/w/<id>`
 * prefix: the caller adds it) and whether it leaves lake. */
export function targetHref(target: Target | null | undefined): { href: string; external: boolean; workspace: boolean } | null {
  if (!target) return null;
  switch (target.kind) {
    case "repository":
      return target.repository_id ? { href: `/repository?${q({ id: target.repository_id })}`, external: false, workspace: false } : null;
    case "repository_path": {
      if (!target.repository_id || !target.path) return null;
      const name = target.path.split("/").pop() ?? "";
      // A name with an extension is a file to preview; anything else is opened as a folder.
      const page = /\.[^.]+$/.test(name) ? "/drive/preview" : "/drive";
      return { href: `${page}?${q({ repository_id: target.repository_id, ref: target.ref || "main", path: target.path })}`, external: false, workspace: false };
    }
    case "drive_entry":
      return target.drive_id && target.entry_id ? { href: `/drives/drive?${q({ id: target.drive_id, focus: target.entry_id })}`, external: false, workspace: true } : null;
    case "dataset": {
      if (!target.id) return null;
      const slash = target.id.indexOf("/");
      const database = slash > 0 ? target.id.slice(0, slash) : "";
      const name = slash > 0 ? target.id.slice(slash + 1) : target.id;
      return { href: `/train/dataset?${q({ name, database })}`, external: false, workspace: true };
    }
    case "transcript":
      return target.id ? { href: `/transcript?${q({ id: target.id })}`, external: false, workspace: true } : null;
    case "project":
      return target.name ? { href: `/design/project?${q({ name: target.name })}`, external: false, workspace: true } : null;
    case "agent":
      return target.id ? { href: `/train/agent?${q({ id: target.id })}`, external: false, workspace: true } : null;
    case "model":
      return target.id ? { href: `/models/${encodeURIComponent(target.id)}`, external: false, workspace: false } : null;
    case "url":
      return target.url && /^https?:\/\//.test(target.url) ? { href: target.url, external: true, workspace: false } : null;
    default:
      return null;
  }
}

/** Media types a browser shows itself and cannot run as a page: lake serves these inline when
 * asked (`inline=1`); anything else downloads. */
export const INLINE_TYPES = new Set([
  "image/png",
  "image/jpeg",
  "image/gif",
  "image/webp",
  "application/pdf",
  "text/plain",
  "text/csv",
  "text/markdown",
  "audio/mpeg",
  "audio/mp4",
  "audio/wav",
  "video/mp4",
  "video/webm",
]);

/** The New shortcut name a person has not typed yet (lake#580): choosing a target fills it in, and
 * focusing it selects it, so typing replaces the suggestion instead of appending to it. Once typed,
 * the name is the person's and a new target leaves it alone. */
export function nameAfterTarget(current: string, typed: boolean, suggested: string | undefined): string {
  if (typed) return current;
  return suggested ?? current;
}

/** Whether focusing the Name field selects its text: only a suggestion nobody has typed over. */
export function selectsOnFocus(name: string, typed: boolean): boolean {
  return !typed && name.length > 0;
}
